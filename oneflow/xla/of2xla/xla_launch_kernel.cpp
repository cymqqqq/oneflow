#include "tensorflow/compiler/xla/client/local_client.h"
#include "tensorflow/compiler/tf2xla/tf2xla_util.h"  // GetXLARandomSeed
#include "tensorflow/compiler/jit/xla_lib/xla_runtime_util.h"
#include "oneflow/xla/of2xla/xla_utility.h"
#include "oneflow/xla/of2xla/xla_graph_compiler.h"
#include "oneflow/xla/of2xla/xla_compilation_cache.h"
#include "oneflow/xla/of2xla/xla_launch_attr.h"
#include "oneflow/xla/of2xla/xla_launch_context.h"
#include "oneflow/xla/of2xla/xla_launch_scope.h"
#include "oneflow/xla/of2xla/xla_launch_kernel.h"

namespace oneflow {

template <DeviceType device_type>
void XlaLaunchKernel<device_type>::BuildLocalExecutable(
    mola::XlaLaunchContext *launch_ctx,
    const std::vector<Blob *> &entry_blobs,
    const std::vector<Blob *> &return_blobs,
    const std::vector<std::string> &entry_blob_names,
    const std::vector<std::string> &return_blob_names,
    const std::vector<xla::XlaBuilder::InputOutputAlias> &aliases,
    mola::CompilationResult **compile_result) const {
  if (!compilation_cache_) {
    compilation_cache_.reset(new mola::XlaCompilationCache);
  }

  const int device_ordinal = launch_ctx->device_ordinal();
  const mola::Signature signature = mola::ComputeSignature(
      launch_ctx->builder()->name(), device_ordinal, entry_blobs);
  bool force_compile = false;
  if (!force_compile) {
    *compile_result = compilation_cache_->GetRecord(signature);
  }

  if (!(*compile_result)) {
    auto result = std::make_shared<mola::CompilationResult>();
    CHECK(this->op_conf().has_xla_launch_conf())
        << "BuildLocalExecutable need a `XlaLaunchOpConf`.";
    const auto &launch_conf = this->op_conf().xla_launch_conf();
    mola::XlaLaunchGraph graph(launch_conf, &this->job_desc());
    mola::XlaGraphCompiler compiler(launch_ctx->client(),
                                    launch_ctx->builder());
    *result = compiler.Compile(&graph, entry_blobs, return_blobs,
                               entry_blob_names, return_blob_names,
                               aliases);
    // Record new compilation result
    compilation_cache_->Record(signature, result);
    // Get compilation result from cache
    *compile_result = compilation_cache_->GetRecord(signature);
  }
}

template <DeviceType device_type>
void XlaLaunchKernel<device_type>::LaunchExecutable(
    mola::XlaLaunchContext *launch_ctx,
    xla::LocalExecutable *executable,
    const std::vector<Blob *> &entry_blobs,
    const std::vector<xla::Shape> &input_shapes,
    std::vector<Blob *> &return_blobs, const xla::Shape &output_shape,
    bool block_host_until_done) const {
  namespace se = tensorflow::se;
  const int device_ordinal = launch_ctx->device_ordinal();
  xla::LocalClient *client = launch_ctx->client();

  CHECK_EQ(entry_blobs.size(), input_shapes.size())
      << "Size mismatch between input blobs and input shapes.";
  CHECK_GT(return_blobs.size(), 0) << "Need one real output at least.";
  // Translate input blobs to xla ShapedBuffer suitable running the executable
  int argument_size = input_shapes.size();
  std::vector<std::shared_ptr<xla::ShapedBuffer>> shaped_buffers(argument_size);
  std::vector<xla::ShapedBuffer *> arguments(argument_size);
  for (int i = 0; i < input_shapes.size(); ++i) {
    const xla::Shape& shape = input_shapes[i];
    const xla::Shape on_device_shape =
        client->backend().transfer_manager()->HostShapeToDeviceShape(shape);
    CHECK(!on_device_shape.IsTuple()) << "Tuple shape is not allowed for input "
                                         "arguments in LaunchExecutable.";
    size_t data_size = entry_blobs[i]->ByteSizeOfDataContentField();
    const char *data_ptr = entry_blobs[i]->dptr<char>();

    // Buffer is nullptr if the blob is body disabled. It should be assigned
    // by a real pointer to prevent check failure while runing the XLA
    // executable, so here we assign the first output buffer to it since it's
    // sure that this entry should never be modified at any time
    if (data_size > 0 && !data_ptr) {
      data_ptr = return_blobs[0]->dptr<char>();
    }
    se::DeviceMemoryBase memory_base =
        se::DeviceMemoryBase(const_cast<char *>(data_ptr), data_size);
    shaped_buffers[i] = std::make_shared<xla::ShapedBuffer>(
        /*on_host_shape=*/shape, /*on_device_shape=*/shape,
        client->platform(), device_ordinal);
    shaped_buffers[i]->set_buffer(memory_base, /*index=*/{});
    arguments[i] = shaped_buffers[i].get();
  }

  OF_CHECK_AND_ASSIGN(auto run_result, [&]() {
    mola::XlaLaunchScope scope(executable, launch_ctx);

    xla::ExecutableRunOptions run_options;
    run_options.set_stream(launch_ctx->stream());
    run_options.set_allocator(launch_ctx->allocator());
    run_options.set_intra_op_thread_pool(launch_ctx->host_device());
    run_options.set_rng_seed(tensorflow::GetXLARandomSeed());

    auto result = executable->RunAsync(arguments, run_options);
    if (block_host_until_done) {
      launch_ctx->stream()->BlockHostUntilDone();
    }
    return std::move(result);
  }());

  // Result shape should be tuple
  CHECK(run_result.on_host_shape().IsTuple());

  // Translate result to output blobs
  for (int i = 0; i < return_blobs.size(); ++i) {
    Blob *output = return_blobs[i];
    se::DeviceMemoryBase buffer = run_result.buffer({i});
    if (buffer.opaque()) {
      CHECK_EQ(buffer.opaque(), output->mut_dptr());
      // Memcpy<device_type>(launch_ctx->device_ctx(), output->mut_dptr(),
      //                     buffer.opaque(),
      //                     output->ByteSizeOfDataContentField());
    }
    // Maybe release result buffer. If we asynchronously launch the executable,
    // then we must not release this buffer here.
    // run_result.set_buffer(se::OwningDeviceMemory(), {i});
  }
}

template <DeviceType device_type>
void XlaLaunchKernel<device_type>::AliasMutableInputsAndOutputs(
    const mola::LaunchAttrHelper &attr,
    const std::vector<Blob *> &entry_blobs,
    const std::vector<std::string> &entry_blob_names,
    std::vector<Blob *> *return_blobs,
    std::vector<std::string> *return_blob_names,
    std::vector<xla::XlaBuilder::InputOutputAlias> *aliases) const {
  CHECK_EQ(entry_blobs.size(), entry_blob_names.size());
  for (int i = 0; i < entry_blobs.size(); ++i) {
    const std::string &entry_name = entry_blob_names[i];
    if (attr.IsMutableArg(entry_name)) {
      aliases->push_back({{static_cast<int>(return_blobs->size())}/*output_index*/,
                          i/*param_number=*/, /*param_index=*/{}});
      return_blobs->push_back(entry_blobs[i]);
      return_blob_names->push_back(attr.OutputArg(entry_name));
    }
  }
}

template <DeviceType device_type>
void XlaLaunchKernel<device_type>::ForwardDataContent(
    const KernelCtx &ctx,
    std::function<Blob*(const std::string&)> BnInOp2Blob) const {
  // Prepare input and output buffers and their names
  std::vector<Blob *> entry_blobs, return_blobs;
  std::vector<std::string> entry_blob_names, return_blob_names;
  for (const auto& input_bn : this->op_attribute().input_bns()) {
    Blob* in_blob = BnInOp2Blob(input_bn);
    entry_blobs.push_back(in_blob);
    const LogicalBlobId& lbi = this->BnInOp2Lbi(input_bn);
    entry_blob_names.push_back(BlobName(lbi));
  }
  for (const auto& output_bn : this->op_attribute().output_bns()) {
    Blob* out_blob = BnInOp2Blob(output_bn);
    return_blobs.push_back(out_blob);
    return_blob_names.push_back(output_bn);
  }
  
  CHECK(this->op_conf().has_xla_launch_conf());
  const auto &launch_conf = this->op_conf().xla_launch_conf();
  mola::LaunchAttrHelper attr_helper(launch_conf.attr());
  std::vector<xla::XlaBuilder::InputOutputAlias> aliases;
  AliasMutableInputsAndOutputs(attr_helper, entry_blobs, entry_blob_names,
                               &return_blobs, &return_blob_names, &aliases);
  
  mola::XlaLaunchContext launch_ctx(this->op_conf().name(), ctx.device_ctx,
                                    device_type, 1 /*intra_op_num_threads*/);
  mola::CompilationResult *compile_result = nullptr;
  BuildLocalExecutable(&launch_ctx, entry_blobs, return_blobs,
                       entry_blob_names, return_blob_names, aliases,
                       &compile_result);
  CHECK(compile_result) << "Executable built failed. "
                        << TF_CPP_VLOG_LEVEL_REQUARED(2);
  auto *executable = compile_result->executable.get();
  
  std::vector<int64_t> allocation_indices;
  xla::ResultAllocationIndices(executable, &allocation_indices);
  CHECK_EQ(return_blobs.size(), allocation_indices.size());
  // Populate output blobs to reuse the buffers in allocator. This helps
  // to reduce memory occupancy and avoid extra copy between temporary
  // buffers and output buffers
  launch_ctx.PopulateResultBuffers(return_blobs, allocation_indices);

  // Launch executable synchronously for CPU, or asynchronously for GPU
  bool block_host_until_done = true;
  if (device_type == DeviceType::kGPU) {
    block_host_until_done = false;
  }
  LaunchExecutable(&launch_ctx, executable, entry_blobs,
                   compile_result->xla_input_shapes, return_blobs,
                   compile_result->xla_output_shape, block_host_until_done);
}

// ADD_DEFAULT_KERNEL_CREATOR(OperatorConf::kXlaLaunchConf, XlaLaunchKernel,
//                            FLOATING_DATA_TYPE_SEQ);
ADD_DEVICE_TYPE_KERNEL_CREATOR(OperatorConf::kXlaLaunchConf, XlaLaunchKernel);

}  // namespace oneflow
