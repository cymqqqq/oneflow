#ifndef ONEFLOW_CORE_MICRO_KERNEL_MICRO_KERNEL_H_
#define ONEFLOW_CORE_MICRO_KERNEL_MICRO_KERNEL_H_

#include "oneflow/core/micro_kernel_graph.h"

namespace oneflow {

template<DeviceType device_type, typename T>
class MicroKernel : public MicroKernelNode {
 public:
  OF_DISALLOW_COPY_AND_MOVE(MicroKernel);
  MicroKernel(const std::vector<BlobSymbol*>& input_blob_symbols,
	      const std::string& out_blob_name,
	      const std::string& out_diff_blob_name)
    : input_blob_symbols_(input_blob_symbols),
      out_blob_name_(out_blob_name), out_diff_blob_name_(out_diff_blob_name) {
    CHECK(!input_blob_symbols_.empty());
    MicroKernelGraph* last_mk_graph = nullptr;
    for (BlobSymbol* input_blob_symbol : input_blob_symbols) {
      MicroKernelGraph* input_mk_graph = input_blob_symbol->mut_mk_graph();
      CHECK(last_mk_graph == nullptr || last_mk_graph == input_mk_graph);
      last_mk_graph = input_mk_graph;
      input_mk_graph->AddAllocatedNode(this);
      MicroKernelNode* input_node = input_blob_symbol->mut_producer_mk_node();
      if (input_node) { Connect(input_node, input_mk_graph->NewEdge(), this); }
    }
  }
  virtual ~MicroKernel() = default;

  virtual void Forward(const KernelCtx& device_ctx,
		       const std::function<Blob*(const std::string&)>& Blob4BnInOp) const = 0;

  virtual void Backward(const KernelCtx& device_ctx,
		       const std::function<Blob*(const std::string&)>& Blob4BnInOp) const = 0;

protected:
  MicroKernelGraph* mut_mk_graph() {
    return input_blob_symbols_.front()->mut_mk_graph();
  }
  const std::vector<BlobSymbol*>& input_blob_symbols() const {
    return input_blob_symbols_;
  }
  const std::string& out_blob_name() const { return out_blob_name_; }
  const std::string& out_diff_blob_name() const { return out_diff_blob_name_; }

 private:
  std::vector<BlobSymbol*> input_blob_symbols_;
  std::string out_blob_name_;
  std::string out_diff_blob_name_;
};


template<typename DerivedT, DeviceType device_type, typename T>
class MicroKernelIf : public MicroKernel<device_type, T> {
 public:
  OF_DISALLOW_COPY_AND_MOVE(MicroKernelIf);
  MicroKernelIf(const std::vector<BlobSymbol*>& input_blob_symbols,
	      const std::string& out_blob_name,
	      const std::string& out_diff_blob_name)
    : MicroKernel(input_blob_symbols, out_blob_name, out_diff_blob_name) {}
  virtual ~MicroKernelIf() = default;

  template<typename... Args>
  static BlobSymbol* Trainable(Args&&... args) {
    return Build(std::forward<Args>(args)...);
  }
  
  template<typename... Args>
  static BlobSymbol* Untrainable(Args&&... args) {
    return Build(std::forward<Args>(args)..., "");
  }

 private:
  template<typename... Args>
  static BlobSymbol* Build(Args&&... args) {
    DerivedT* mk = new DerivedT(std::forward<Args>(args)...);
    return mut_mk_graph()->NewBlobSymbol(mk, mk->out_blob_name(),
					 mk->out_diff_blob_name());
  }
  
};

}

#endif  // ONEFLOW_CORE_MICRO_KERNEL_MICRO_KERNEL_H_
