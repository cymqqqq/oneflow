#ifndef ONEFLOW_CORE_OPERATOR_PROPOSAL_TARGET_OP_H_
#define ONEFLOW_CORE_OPERATOR_PROPOSAL_TARGET_OP_H_

#include "oneflow/core/operator/operator.h"

namespace oneflow {

class ProposalTargetOp final : public Operator {
 public:
  OF_DISALLOW_COPY_AND_MOVE(ProposalTargetOp);
  ProposalTargetOp() = default;
  ~ProposalTargetOp() = default;

  void InitFromOpConf() override;
  const PbMessage& GetCustomizedConf() const override;
  void InferBlobDescs(std::function<BlobDesc*(const std::string&)> GetBlobDesc4BnInOp,
                      const ParallelContext* parallel_ctx) const override;
};

}  // namespace oneflow

#endif  // ONEFLOW_CORE_OPERATOR_PROPOSAL_TARGET_OP_H_
