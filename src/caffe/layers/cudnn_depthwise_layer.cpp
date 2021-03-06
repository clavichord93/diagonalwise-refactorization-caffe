#ifdef USE_CUDNN
#include <algorithm>
#include <vector>

#include "caffe/layers/cudnn_depthwise_layer.hpp"

namespace caffe {

// Set to three for the benefit of the backward pass, which
// can use separate streams for calculating the gradient w.r.t.
// bias, filter weights, and bottom data for each group independently
#define CUDNN_STREAMS_DEPTHWISE 3

/**
 * TODO(dox) explain cuDNN interface
 */
template <typename Dtype>
void CuDNNDepthwiseLayer<Dtype>::LayerSetUp(
    const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  DepthwiseLayer<Dtype>::LayerSetUp(bottom, top);

  // Initialize group_ and weight_offset_.
  group_ = this->layer_param_.convolution_param().group();
  //group_ = std::min(64, this->channels_);
  //group_ = 32;
  //group_ = std::max(this->channels_ / 32, 1);
  CHECK_EQ(0, this->channels_ % group_)
      << "CuDNNConvolution input channels must be divisible by groups.";
  const int* kernel_shape_data = this->kernel_shape_.cpu_data();
  const int kernel_h = kernel_shape_data[0];
  const int kernel_w = kernel_shape_data[1];
  weight_offset_ = (this->num_output_ / group_) * (this->channels_ / group_) * 
      kernel_h * kernel_w;

  // Initialize CUDA streams and cuDNN.
  stream_ = new cudaStream_t[CUDNN_STREAMS_DEPTHWISE];
  handle_ = new cudnnHandle_t[CUDNN_STREAMS_DEPTHWISE];

  // Initialize algorithm arrays
  fwd_algo_ = new cudnnConvolutionFwdAlgo_t[bottom.size()];
  bwd_filter_algo_ = new cudnnConvolutionBwdFilterAlgo_t[bottom.size()];
  bwd_data_algo_ = new cudnnConvolutionBwdDataAlgo_t[bottom.size()];

  // initialize size arrays
  workspace_fwd_sizes_ = new size_t[bottom.size()];
  workspace_bwd_filter_sizes_ = new size_t[bottom.size()];
  workspace_bwd_data_sizes_ = new size_t[bottom.size()];

  // workspace data
  workspaceSizeInBytes = 0;
  workspaceData = NULL;
  workspace = new void*[CUDNN_STREAMS_DEPTHWISE];

  for (size_t i = 0; i < bottom.size(); ++i) {
    // initialize all to default algorithms
    fwd_algo_[i] = (cudnnConvolutionFwdAlgo_t)0;
    bwd_filter_algo_[i] = (cudnnConvolutionBwdFilterAlgo_t)0;
    bwd_data_algo_[i] = (cudnnConvolutionBwdDataAlgo_t)0;
    // default algorithms don't require workspace
    workspace_fwd_sizes_[i] = 0;
    workspace_bwd_data_sizes_[i] = 0;
    workspace_bwd_filter_sizes_[i] = 0;
  }

  for (int g = 0; g < CUDNN_STREAMS_DEPTHWISE; g++) {
    CUDA_CHECK(cudaStreamCreate(&stream_[g]));
    CUDNN_CHECK(cudnnCreate(&handle_[g]));
    CUDNN_CHECK(cudnnSetStream(handle_[g], stream_[g]));
    workspace[g] = NULL;
  }

  // Create filter descriptor.
  cudnn::createFilterDesc<Dtype>(&filter_desc_,
      this->num_output_ / group_, this->channels_ / group_, kernel_h, kernel_w);

  // Create tensor descriptor(s) for data and corresponding convolution(s).
  for (int i = 0; i < bottom.size(); i++) {
    cudnnTensorDescriptor_t bottom_desc;
    cudnn::createTensor4dDesc<Dtype>(&bottom_desc);
    bottom_descs_.push_back(bottom_desc);
    cudnnTensorDescriptor_t top_desc;
    cudnn::createTensor4dDesc<Dtype>(&top_desc);
    top_descs_.push_back(top_desc);
    cudnnConvolutionDescriptor_t conv_desc;
    cudnn::createConvolutionDesc<Dtype>(&conv_desc);
    conv_descs_.push_back(conv_desc);
  }

  // Tensor descriptor for bias.
  if (this->bias_term_) {
    cudnn::createTensor4dDesc<Dtype>(&bias_desc_);
  }

  // Modify parameters in DepthwiseLayer to CuDNNDepthwiseLayer
  vector<int> weight_shape(4);
  weight_shape[0] = this->num_output_;
  weight_shape[1] = this->channels_ / group_;
  weight_shape[2] = kernel_h;
  weight_shape[3] = kernel_w;

  caffe_weight_.CopyFrom(*this->blobs_[0], false, true);
  caffe_weight_.CopyFrom(*this->blobs_[0], true, false);
  this->blobs_[0]->Reshape(weight_shape);
  mask_.Reshape(weight_shape);

  CaffeToCuDNN();

  handles_setup_ = true;
}

template <typename Dtype>
void CuDNNDepthwiseLayer<Dtype>::Reshape(
    const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  DepthwiseLayer<Dtype>::Reshape(bottom, top);
  CHECK_EQ(2, this->num_spatial_axes_)
      << "CuDNNConvolution input must have 2 spatial axes "
      << "(e.g., height and width). "
      << "Use 'engine: CAFFE' for general ND convolution.";
  bottom_offset_ = this->bottom_dim_ / this->group_;
  top_offset_ = this->top_dim_ / this->group_;
  const int height = bottom[0]->shape(this->channel_axis_ + 1);
  const int width = bottom[0]->shape(this->channel_axis_ + 2);
  const int height_out = top[0]->shape(this->channel_axis_ + 1);
  const int width_out = top[0]->shape(this->channel_axis_ + 2);
  const int* pad_data = this->pad_.cpu_data();
  const int pad_h = pad_data[0];
  const int pad_w = pad_data[1];
  const int* stride_data = this->stride_.cpu_data();
  const int stride_h = stride_data[0];
  const int stride_w = stride_data[1];

  // Specify workspace limit for kernels directly until we have a
  // planning strategy and a rewrite of Caffe's GPU memory mangagement
  size_t workspace_limit_bytes = 8*1024*1024;

  for (int i = 0; i < bottom.size(); i++) {
    cudnn::setTensor4dDesc<Dtype>(&bottom_descs_[i],
        this->num_, this->channels_ / group_ , height, width,
        this->channels_ * height * width,
        height * width, width, 1);
    cudnn::setTensor4dDesc<Dtype>(&top_descs_[i],
        this->num_, this->num_output_ / group_, height_out, width_out,
        this->num_output_ * this->out_spatial_dim_,
        this->out_spatial_dim_, width_out, 1);
    cudnn::setConvolutionDesc<Dtype>(&conv_descs_[i], bottom_descs_[i],
        filter_desc_, pad_h, pad_w, stride_h, stride_w);

    // choose forward and backward algorithms + workspace(s)
    CUDNN_CHECK(cudnnGetConvolutionForwardAlgorithm(handle_[0],
        bottom_descs_[i], filter_desc_, conv_descs_[i], top_descs_[i],
        CUDNN_CONVOLUTION_FWD_SPECIFY_WORKSPACE_LIMIT, workspace_limit_bytes,
        &fwd_algo_[i]));

    CUDNN_CHECK(cudnnGetConvolutionForwardWorkspaceSize(handle_[0],
        bottom_descs_[i], filter_desc_, conv_descs_[i], top_descs_[i],
        fwd_algo_[i], &(workspace_fwd_sizes_[i])));

    // choose backward algorithm for filter
    CUDNN_CHECK(cudnnGetConvolutionBackwardFilterAlgorithm(handle_[0],
        bottom_descs_[i], top_descs_[i], conv_descs_[i], filter_desc_,
        CUDNN_CONVOLUTION_BWD_FILTER_SPECIFY_WORKSPACE_LIMIT,
        workspace_limit_bytes, &bwd_filter_algo_[i]) );

    // get workspace for backwards filter algorithm
    CUDNN_CHECK(cudnnGetConvolutionBackwardFilterWorkspaceSize(handle_[0],
        bottom_descs_[i], top_descs_[i], conv_descs_[i], filter_desc_,
        bwd_filter_algo_[i], &workspace_bwd_filter_sizes_[i]));

    // choose backward algo for data
    CUDNN_CHECK(cudnnGetConvolutionBackwardDataAlgorithm(handle_[0],
        filter_desc_, top_descs_[i], conv_descs_[i], bottom_descs_[i],
        CUDNN_CONVOLUTION_BWD_DATA_SPECIFY_WORKSPACE_LIMIT,
        workspace_limit_bytes, &bwd_data_algo_[i]));

    // get workspace size
    CUDNN_CHECK(cudnnGetConvolutionBackwardDataWorkspaceSize(handle_[0],
        filter_desc_, top_descs_[i], conv_descs_[i], bottom_descs_[i],
        bwd_data_algo_[i], &workspace_bwd_data_sizes_[i]) );
  }

  // reduce over all workspace sizes to get a maximum to allocate / reallocate
  size_t total_workspace_fwd = 0;
  size_t total_workspace_bwd_data = 0;
  size_t total_workspace_bwd_filter = 0;

  for (size_t i = 0; i < bottom.size(); i++) {
    total_workspace_fwd = std::max(total_workspace_fwd,
        workspace_fwd_sizes_[i]);
    total_workspace_bwd_data = std::max(total_workspace_bwd_data,
        workspace_bwd_data_sizes_[i]);
    total_workspace_bwd_filter = std::max(total_workspace_bwd_filter,
        workspace_bwd_filter_sizes_[i]);
  }
  // get max over all operations
  size_t max_workspace = std::max(total_workspace_fwd,
      total_workspace_bwd_data);
  max_workspace = std::max(max_workspace, total_workspace_bwd_filter);
  // ensure all groups have enough workspace
  size_t total_max_workspace = max_workspace * CUDNN_STREAMS_DEPTHWISE;

  // this is the total amount of storage needed over all groups + streams
  if (total_max_workspace > workspaceSizeInBytes) {
    DLOG(INFO) << "Reallocating workspace storage: " << total_max_workspace;
    workspaceSizeInBytes = total_max_workspace;

    // free the existing workspace and allocate a new (larger) one
    cudaFree(this->workspaceData);

    cudaError_t err = cudaMalloc(&(this->workspaceData), workspaceSizeInBytes);
    if (err != cudaSuccess) {
      // force zero memory path
      for (int i = 0; i < bottom.size(); i++) {
        workspace_fwd_sizes_[i] = 0;
        workspace_bwd_filter_sizes_[i] = 0;
        workspace_bwd_data_sizes_[i] = 0;
        fwd_algo_[i] = CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM;
        bwd_filter_algo_[i] = CUDNN_CONVOLUTION_BWD_FILTER_ALGO_0;
        bwd_data_algo_[i] = CUDNN_CONVOLUTION_BWD_DATA_ALGO_0;
      }

      // NULL out all workspace pointers
      for (int g = 0; g < CUDNN_STREAMS_DEPTHWISE; g++) {
        workspace[g] = NULL;
      }
      // NULL out underlying data
      workspaceData = NULL;
      workspaceSizeInBytes = 0;
    }

    // if we succeed in the allocation, set pointer aliases for workspaces
    for (int g = 0; g < CUDNN_STREAMS_DEPTHWISE; g++) {
      workspace[g] = reinterpret_cast<char*>(workspaceData) + g * max_workspace;
    }
  }

  // Tensor descriptor for bias.
  if (this->bias_term_) {
    cudnn::setTensor4dDesc<Dtype>(&bias_desc_, 1, this->num_output_, 1, 1);
  }
}

template <typename Dtype>
CuDNNDepthwiseLayer<Dtype>::~CuDNNDepthwiseLayer() {
  // Check that handles have been setup before destroying.
  if (!handles_setup_) { return; }

  for (int i = 0; i < bottom_descs_.size(); i++) {
    cudnnDestroyTensorDescriptor(bottom_descs_[i]);
    cudnnDestroyTensorDescriptor(top_descs_[i]);
    cudnnDestroyConvolutionDescriptor(conv_descs_[i]);
  }
  if (this->bias_term_) {
    cudnnDestroyTensorDescriptor(bias_desc_);
  }
  cudnnDestroyFilterDescriptor(filter_desc_);

  for (int g = 0; g < CUDNN_STREAMS_DEPTHWISE; g++) {
    cudaStreamDestroy(stream_[g]);
    cudnnDestroy(handle_[g]);
  }

  cudaFree(workspaceData);
  delete [] workspace;
  delete [] stream_;
  delete [] handle_;
  delete [] fwd_algo_;
  delete [] bwd_filter_algo_;
  delete [] bwd_data_algo_;
  delete [] workspace_fwd_sizes_;
  delete [] workspace_bwd_data_sizes_;
  delete [] workspace_bwd_filter_sizes_;
}

template <typename Dtype>
void CuDNNDepthwiseLayer<Dtype>::ToProto(LayerParameter* param,
    bool write_diff) {
  param->Clear();
  param->CopyFrom(this->layer_param_);
  param->clear_blobs();

  if (this->blobs_.size() > 0) {
    CuDNNToCaffe();

    caffe_weight_.ToProto(param->add_blobs(), write_diff);
    for (int i = 1; i < this->blobs_.size(); i++) {
      this->blobs_[i]->ToProto(param->add_blobs(), write_diff);
    }
  }
}

template <typename Dtype>
void CuDNNDepthwiseLayer<Dtype>::CaffeToCuDNN() {
  if (this->blobs_.size() > 0) {
    const Dtype* caffe_data = caffe_weight_.cpu_data();
    const Dtype* caffe_diff = caffe_weight_.cpu_diff();
    Dtype* cudnn_data = this->blobs_[0]->mutable_cpu_data();
    Dtype* cudnn_diff = this->blobs_[0]->mutable_cpu_diff();
    Dtype* mask_data = mask_.mutable_cpu_data();
    int channels_per_group = this->channels_ / group_;

    int N = this->num_output_ * channels_per_group * this->kernel_dim_;
    caffe_set(N, (Dtype)0., cudnn_data);
    caffe_set(N, (Dtype)0., cudnn_diff);
    caffe_set(N, (Dtype)0., mask_data);

    for (int i = 0; i < this->num_output_; ++i) {
      for (int k = 0; k < this->kernel_dim_; ++k) {
        int j = i / this->multiplier_ % channels_per_group;
        int idx_cudnn = (i * channels_per_group + j) * this->kernel_dim_ + k;
        int idx_caffe = i * this->kernel_dim_ + k;
        cudnn_data[idx_cudnn] = caffe_data[idx_caffe];
        cudnn_diff[idx_cudnn] = caffe_diff[idx_caffe];
        mask_data[idx_cudnn] = 1.;
      }
    }
  }
}

template <typename Dtype>
void CuDNNDepthwiseLayer<Dtype>::CuDNNToCaffe() {
  if (this->blobs_.size() > 0) {
    const Dtype* cudnn_data = this->blobs_[0]->cpu_data();
    const Dtype* cudnn_diff = this->blobs_[0]->cpu_diff();
    Dtype* caffe_data = caffe_weight_.mutable_cpu_data();
    Dtype* caffe_diff = caffe_weight_.mutable_cpu_diff();
    int channels_per_group = this->channels_ / group_;
    for (int i = 0; i < this->num_output_; ++i) {
      for (int k = 0; k < this->kernel_dim_; ++k) {
        int j = i / this->multiplier_ % channels_per_group;
        int idx_cudnn = (i * channels_per_group + j) * this->kernel_dim_ + k;
        int idx_caffe = i * this->kernel_dim_ + k;
        caffe_data[idx_caffe] = cudnn_data[idx_cudnn];
        caffe_diff[idx_caffe] = cudnn_diff[idx_cudnn];
      }
    }
  }
}

INSTANTIATE_CLASS(CuDNNDepthwiseLayer);

}   // namespace caffe
#endif
