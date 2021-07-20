#include <torch/extension.h>
#include <c10/cuda/CUDAStream.h>
#include <sputnik/sputnik.h>

//
/// Validation helpers.
//

#define CALL_CUDA(code)					    \
  do {                                                      \
    cudaError_t status = code;                              \
    std::string err = cudaGetErrorString(status);           \
    TORCH_CHECK(status == cudaSuccess, err);		    \
  } while (0)

#define CHECK_CUDA(x) TORCH_CHECK(x.is_cuda())
#define CHECK_CPU(x) TORCH_CHECK(!x.is_cuda())
#define CHECK_HALF(x) TORCH_CHECK(x.scalar_type() == torch::ScalarType::Half)
#define CHECK_INT(x) TORCH_CHECK(x.scalar_type() == torch::ScalarType::Int)
#define CHECK_SHORT(x) TORCH_CHECK(x.scalar_type() == torch::ScalarType::Short)
#define CHECK_SCALAR(x) TORCH_CHECK(x.numel() == 1)
#define CHECK_VECTOR(x) TORCH_CHECK(x.ndimension() == 1)
#define CHECK_MATRIX(x) TORCH_CHECK(x.ndimension() == 2)
#define CHECK_3D(x) TORCH_CHECK(x.ndimension() == 3)
#define CHECK_SHAPE(x) TORCH_CHECK(x.numel() == 2)

void validate_shape(torch::Tensor shape) {
  CHECK_CPU(shape);
  CHECK_SHAPE(shape);
  CHECK_INT(shape);
}

void validate_transpose(torch::Tensor transpose) {
  CHECK_CPU(transpose);
  CHECK_SCALAR(transpose);
  CHECK_INT(transpose);
}

void validate_sparse(torch::Tensor shape,
		     torch::Tensor data,
		     torch::Tensor offsets,
		     torch::Tensor indices) {
  validate_shape(shape);
  CHECK_CUDA(data);
  CHECK_3D(data);
  CHECK_HALF(data);
  CHECK_CUDA(offsets);
  CHECK_VECTOR(offsets);
  CHECK_INT(offsets);
  CHECK_CUDA(indices);
  CHECK_VECTOR(indices);
  CHECK_SHORT(indices);

  // Blocking must be square.
  TORCH_CHECK(data.size(1) == data.size(2));

  // TODO(tgale): Generalize this.
  TORCH_CHECK(data.size(1) == 128);
}

bool is_transposed(torch::Tensor x) {
  return x.strides()[0] == 1 && x.strides()[1] == x.sizes()[0];
}

void validate_dense(torch::Tensor x) {
  CHECK_CUDA(x);
  CHECK_MATRIX(x);
  CHECK_HALF(x);

  TORCH_CHECK(x.is_contiguous() || is_transposed(x));
}

//
/// Conversion helpers.
//

sputnik::block::Matrix as_matrix(torch::Tensor x) {
  validate_dense(x);
  int rows = is_transposed(x) ? x.sizes()[1] : x.sizes()[0];
  int cols = is_transposed(x) ? x.sizes()[0] : x.sizes()[1];
  return sputnik::block::Matrix(rows, cols, x.data_ptr());
}

int access_metadata(torch::Tensor m, int idx = 0) {
  auto accessor = m.accessor<int, 1>();
  return accessor[idx];
}

sputnik::block::BlockMatrix as_block_matrix(torch::Tensor shape,
					    torch::Tensor data,
					    torch::Tensor offsets,
					    torch::Tensor indices) {
  validate_sparse(shape, data, offsets, indices);
  return sputnik::block::BlockMatrix(access_metadata(shape, 0),
				     access_metadata(shape, 1),
				     sputnik::block::AsBlockSize(data.size(1)),
				     indices.numel(),
				     data.data_ptr(),
				     offsets.data_ptr(),
				     indices.data_ptr());
}

//
/// Row indices helper.
//

torch::Tensor row_indices(torch::Tensor shape,
			  torch::Tensor data,
			  torch::Tensor offsets,
			  torch::Tensor indices) {
  auto x = as_block_matrix(shape, data, offsets, indices);
  auto options = torch::TensorOptions()
    .dtype(torch::kInt16)
    .device(data.device());
  auto out = torch::empty(indices.size(0), options);
  CALL_CUDA(sputnik::block::RowIndices(x,
				       out.data_ptr<short>(),
				       c10::cuda::getCurrentCUDAStream()));
  return out;
}

//
/// Transpose helper.
//

std::vector<torch::Tensor> transpose(torch::Tensor shape,
				     torch::Tensor data,
				     torch::Tensor offsets,
				     torch::Tensor indices) {
  const int kNonzeros = indices.numel();
  const int kBlockSize = data.size(1);
  const int kCols = access_metadata(shape, 1);
  const int kBlockCols = kCols / kBlockSize;

  // Sort row indices by column indices to get the transposed
  // matrix's column indices.
  //
  // TODO(tgale): Replace the hacky offset with a stable sort
  // when it's available.
  TORCH_CHECK(kBlockSize == 128);
  TORCH_CHECK(access_metadata(shape, 0) <= (128*128));
  torch::Tensor row_idxs = row_indices(shape, data, offsets, indices);
  torch::Tensor sort_indices = indices + row_idxs / kBlockSize;
  torch::Tensor gather_indices = sort_indices.argsort();
  torch::Tensor indices_t = row_idxs.gather(0, gather_indices);

  // Sort block offsets by column indices to get the transposed
  // matrix's block locations for each block row.
  const int kValuesPerBlock = kBlockSize * kBlockSize;
  const int kBytesPerBlock = kValuesPerBlock * sizeof(sputnik::half);

  auto options = torch::TensorOptions()
    .dtype(torch::kInt32)
    .device(data.device());

  torch::Tensor block_offsets = torch::linspace(0,
						(kNonzeros - 1) * kBytesPerBlock,
						kNonzeros,
						options);
  torch::Tensor block_offsets_t = block_offsets.gather(0, gather_indices);

  // Calculate the transposed matrix's offsets.
  torch::Tensor nnz_per_column = indices.histc(kBlockCols, 0, kCols);
  torch::Tensor zero = torch::zeros(1, options);
  torch::Tensor offsets_t = at::cat({zero, nnz_per_column.cumsum(0) * kValuesPerBlock});
  offsets_t = offsets_t.to(options);
  return {indices_t, offsets_t, block_offsets_t};
}

void standardize_shape(torch::Tensor shape, bool transpose) {
  int rows = transpose ? access_metadata(shape, 1) : access_metadata(shape, 0);
  int cols = transpose ? access_metadata(shape, 0) : access_metadata(shape, 1);
  shape.data_ptr<int>()[0] = rows;
  shape.data_ptr<int>()[1] = cols;
}

//
/// Bitmask helper.
//

size_t size_in_bytes(int rows, int columns) {
  const int kAlignment = 64;
  int column_entries = (columns + kAlignment - 1) / kAlignment;
  return column_entries * rows * sizeof(uint64_t);
}

torch::Tensor allocate_bitmask(sputnik::block::BlockMatrix m, bool trans, torch::Device device) {
  int block_size = AsInt(m.block_size);
  int block_rows = (trans ? m.cols : m.rows) / block_size;
  int block_cols = (trans ? m.rows : m.cols) / block_size;
  size_t bytes = size_in_bytes(block_rows, block_cols);

  auto options = torch::TensorOptions()
    .dtype(torch::kInt8)
    .device(device);
  return torch::empty(bytes, options);
}

//
/// Custom operations.
//


void dsd(torch::Tensor shape,
	 torch::Tensor data,
	 torch::Tensor offsets,
	 torch::Tensor indices,
	 torch::Tensor transpose_a,
	 torch::Tensor rhs_t,
	 torch::Tensor out_t) {
  // Convert the arguments to sputnik types.
  validate_transpose(transpose_a);
  bool transpose_lhs = access_metadata(transpose_a);
  validate_shape(shape);
  standardize_shape(shape, transpose_lhs);
  auto lhs = as_block_matrix(shape,
			     data,
			     offsets,
			     indices);
  auto rhs = as_matrix(rhs_t);
  bool transpose_rhs = is_transposed(rhs_t);
  auto out = as_matrix(out_t);

  // Validate the problem configuration.
  TORCH_CHECK(sputnik::block::ValidMatmul(lhs,
					  transpose_lhs,
					  rhs,
					  transpose_rhs,
					  out));

  if (transpose_lhs) {
    // Populate the transpose meta-data.
    auto meta = transpose(shape, data, offsets, indices);

    // Set the data pointers.
    lhs.indices_t = meta[0].data_ptr();
    lhs.offsets_t = meta[1].data_ptr();
    lhs.block_offsets = meta[2].data_ptr();
    CALL_CUDA(sputnik::block::MatmulEx(lhs,
				       transpose_lhs,
				       rhs,
				       transpose_rhs,
				       out,
				       c10::cuda::getCurrentCUDAStream()));
  } else {
    CALL_CUDA(sputnik::block::Matmul(lhs,
				     transpose_lhs,
				     rhs,
				     transpose_rhs,
				     out,
				     c10::cuda::getCurrentCUDAStream()));
  }
}

void dds(torch::Tensor lhs_t,
	 torch::Tensor shape,
	 torch::Tensor data,
	 torch::Tensor offsets,
	 torch::Tensor indices,
	 torch::Tensor transpose_b,
	 torch::Tensor out_t) {
  // Convert the arguments to sputnik types.
  auto lhs = as_matrix(lhs_t);
  bool transpose_lhs = is_transposed(lhs_t);
  validate_transpose(transpose_b);
  bool transpose_rhs = access_metadata(transpose_b);
  validate_shape(shape);
  standardize_shape(shape, transpose_rhs);
  auto rhs = as_block_matrix(shape,
			     data,
			     offsets,
			     indices);
  auto out = as_matrix(out_t);

  // Validate the problem configuration.
  TORCH_CHECK(sputnik::block::ValidMatmul(lhs,
					  transpose_lhs,
					  rhs,
					  transpose_rhs,
					  out));

  if (!transpose_rhs) {
    // Populate the transpose meta-data.
    auto meta = transpose(shape, data, offsets, indices);

    // Set the data pointers.
    rhs.indices_t = meta[0].data_ptr();
    rhs.offsets_t = meta[1].data_ptr();
    rhs.block_offsets = meta[2].data_ptr();
    CALL_CUDA(sputnik::block::MatmulEx(lhs,
				       transpose_lhs,
				       rhs,
				       transpose_rhs,
				       out,
				       c10::cuda::getCurrentCUDAStream()));
  } else {
    CALL_CUDA(sputnik::block::Matmul(lhs,
				     transpose_lhs,
				     rhs,
				     transpose_rhs,
				     out,
				     c10::cuda::getCurrentCUDAStream()));
  }
}

void sdd(torch::Tensor lhs_t,
	 torch::Tensor rhs_t,
	 torch::Tensor shape,
	 torch::Tensor data,
	 torch::Tensor offsets,
	 torch::Tensor indices) {
  // Convert the arguments to sputnik types.
  auto lhs = as_matrix(lhs_t);
  bool transpose_lhs = is_transposed(lhs_t);
  auto rhs = as_matrix(rhs_t);
  bool transpose_rhs = is_transposed(rhs_t);
  auto out = as_block_matrix(shape,
			     data,
			     offsets,
			     indices);

  // Validate the problem configuration.
  TORCH_CHECK(sputnik::block::ValidMatmul(lhs,
					  transpose_lhs,
					  rhs,
					  transpose_rhs,
					  out));

  CALL_CUDA(sputnik::block::Matmul(lhs,
				   transpose_lhs,
				   rhs,
				   transpose_rhs,
				   out,
				   c10::cuda::getCurrentCUDAStream()));
}

void ssd(torch::Tensor lhs_shape,
	 torch::Tensor lhs_data,
	 torch::Tensor lhs_offsets,
	 torch::Tensor lhs_indices,
	 torch::Tensor transpose_a,
	 torch::Tensor rhs_t,
	 torch::Tensor out_shape,
	 torch::Tensor out_data,
	 torch::Tensor out_offsets,
	 torch::Tensor out_indices) {
  // Convert the arguments to sputnik types.
  validate_transpose(transpose_a);
  bool transpose_lhs = access_metadata(transpose_a);
  validate_shape(lhs_shape);
  standardize_shape(lhs_shape, transpose_lhs);
  auto lhs = as_block_matrix(lhs_shape,
			     lhs_data,
			     lhs_offsets,
			     lhs_indices);

  auto rhs = as_matrix(rhs_t);
  bool transpose_rhs = is_transposed(rhs_t);

  auto out = as_block_matrix(out_shape,
			     out_data,
			     out_offsets,
			     out_indices);

  // Validate the problem configuration.
  TORCH_CHECK(sputnik::block::ValidMatmul(lhs,
					  transpose_lhs,
					  rhs,
					  transpose_rhs,
					  out));

  if (transpose_lhs) {
    // Populate the transpose meta-data.
    auto meta = transpose(lhs_shape, lhs_data, lhs_offsets, lhs_indices);

    // Set the data pointers.
    lhs.indices_t = meta[0].data_ptr();
    lhs.offsets_t = meta[1].data_ptr();
    lhs.block_offsets = meta[2].data_ptr();
    CALL_CUDA(sputnik::block::MatmulEx(lhs,
				       transpose_lhs,
				       rhs,
				       transpose_rhs,
				       out,
				       c10::cuda::getCurrentCUDAStream()));
  } else {
    CALL_CUDA(sputnik::block::Matmul(lhs,
				     transpose_lhs,
				     rhs,
				     transpose_rhs,
				     out,
				     c10::cuda::getCurrentCUDAStream()));
  }
}

void dss(torch::Tensor lhs_shape,
	 torch::Tensor lhs_data,
	 torch::Tensor lhs_offsets,
	 torch::Tensor lhs_indices,
	 torch::Tensor transpose_a,
	 torch::Tensor rhs_shape,
	 torch::Tensor rhs_data,
	 torch::Tensor rhs_offsets,
	 torch::Tensor rhs_indices,
	 torch::Tensor transpose_b,
	 torch::Tensor out_t) {
  // Convert the arguments to sputnik types.
  validate_transpose(transpose_a);
  bool transpose_lhs = access_metadata(transpose_a);
  validate_shape(lhs_shape);
  standardize_shape(lhs_shape, transpose_lhs);
  auto lhs = as_block_matrix(lhs_shape,
			     lhs_data,
			     lhs_offsets,
			     lhs_indices);

  validate_transpose(transpose_b);
  bool transpose_rhs = access_metadata(transpose_b);
  validate_shape(rhs_shape);
  standardize_shape(rhs_shape, transpose_rhs);
  auto rhs = as_block_matrix(rhs_shape,
			     rhs_data,
			     rhs_offsets,
			     rhs_indices);
  auto out = as_matrix(out_t);

  // Validate the problem configuration.
  TORCH_CHECK(sputnik::block::ValidMatmul(lhs,
					  transpose_lhs,
					  rhs,
					  transpose_rhs,
					  out));

  // Allocate workspace for the bitmasks.
  auto lhs_bitmask = allocate_bitmask(lhs, transpose_lhs, lhs_data.device());
  lhs.bitmask = lhs_bitmask.data_ptr();
  auto rhs_bitmask = allocate_bitmask(rhs, transpose_rhs, rhs_data.device());
  rhs.bitmask = rhs_bitmask.data_ptr();

  // Handle exposed transposes.
  std::vector<torch::Tensor> lhs_meta, rhs_meta;
  if (transpose_lhs) {
    // Populate the transpose meta-data.
    lhs_meta = transpose(lhs_shape, lhs_data, lhs_offsets, lhs_indices);

    // Set the data pointers.
    lhs.indices_t = lhs_meta[0].data_ptr();
    lhs.offsets_t = lhs_meta[1].data_ptr();
    lhs.block_offsets = lhs_meta[2].data_ptr();
  }
  if (!transpose_rhs) {
    // Populate the transpose meta-data.
    rhs_meta = transpose(rhs_shape, rhs_data, rhs_offsets, rhs_indices);

    // Set the data pointers.
    rhs.indices_t = rhs_meta[0].data_ptr();
    rhs.offsets_t = rhs_meta[1].data_ptr();
    rhs.block_offsets = rhs_meta[2].data_ptr();
  }

  CALL_CUDA(sputnik::block::MatmulEx(lhs,
				     transpose_lhs,
				     rhs,
				     transpose_rhs,
				     out,
				     c10::cuda::getCurrentCUDAStream()));
}

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
  m.def("dsd", &dsd, "dense = op(sparse) x op(dense)");
  m.def("dds", &dds, "dense = op(dense) x op(sparse)");
  m.def("sdd", &sdd, "sparse = op(dense) x op(dense)");
  m.def("ssd", &ssd, "sparse = op(dense) x op(sparse)");
  m.def("dss", &dss, "dense = op(sparse) x op(sparse)");
}