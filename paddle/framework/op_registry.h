#pragma once

#include <algorithm>
#include <atomic>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include "paddle/framework/attr_checker.h"
#include "paddle/framework/op_desc.pb.h"
#include "paddle/framework/op_proto.pb.h"
#include "paddle/framework/operator.h"
#include "paddle/framework/scope.h"

namespace paddle {
namespace framework {

// helper class to set attribute type
struct AttrTypeHelper {
  template <typename T>
  static void SetAttrType(AttrProto* attr);

  static Attribute GetAttrValue(const AttrDesc& attr_desc) {
    switch (attr_desc.type()) {
      case paddle::framework::AttrType::INT: {
        return attr_desc.i();
      }
      case paddle::framework::AttrType::FLOAT: {
        return attr_desc.f();
      }
      case paddle::framework::AttrType::STRING: {
        return attr_desc.s();
      }
      case paddle::framework::AttrType::INTS: {
        std::vector<int> val(attr_desc.ints_size());
        for (int i = 0; i < attr_desc.ints_size(); ++i) {
          val[i] = attr_desc.ints(i);
        }
        return val;
      }
      case paddle::framework::AttrType::FLOATS: {
        std::vector<float> val(attr_desc.floats_size());
        for (int i = 0; i < attr_desc.floats_size(); ++i) {
          val[i] = attr_desc.floats(i);
        }
        return val;
      }
      case paddle::framework::AttrType::STRINGS: {
        std::vector<std::string> val(attr_desc.strings_size());
        for (int i = 0; i < attr_desc.strings_size(); ++i) {
          val[i] = attr_desc.strings(i);
        }
        return val;
      }
    }
    PADDLE_ENFORCE(false, "Unknown OpDesc::AttrDesc::type !");
    return boost::blank();
  }
};

// this class not only make proto but also init attribute checkers.
class OpProtoAndCheckerMaker {
 public:
  OpProtoAndCheckerMaker(OpProto* proto, OpAttrChecker* op_checker)
      : proto_(proto), op_checker_(op_checker) {}

  ~OpProtoAndCheckerMaker() {
    PADDLE_ENFORCE(validated_, "should call Validate after build");
  }

  void Validate() {
    validated_ = true;
    CheckNoDuplicatedInOutAttrs();
  }

 protected:
  void AddInput(const std::string& name, const std::string& comment,
                bool multiple = false) {
    auto input = proto_->mutable_inputs()->Add();
    *input->mutable_name() = name;
    *input->mutable_comment() = comment;
    input->set_multiple(multiple);
    if (multiple) {
      SetHasMultipleInput();
    }
  }

  void AddInputs(const std::string& name, const std::string& comment) {
    AddInput(name, comment, true);
  }

  void AddOutput(const std::string& name, const std::string& comment,
                 bool temporary = false, bool multiple = false) {
    auto output = proto_->mutable_outputs()->Add();
    *output->mutable_name() = name;
    *output->mutable_comment() = comment;
    output->set_multiple(multiple);
    if (multiple) {
      SetHasMultipleOutput();
    }
    output->set_temporary(temporary);
    if (temporary) {
      SetHasTemporaryOutput();
    }
  }

  void AddOutputs(const std::string& name, const std::string& comment,
                  bool temporary = false) {
    AddOutput(name, comment, temporary, true);
  }

  template <typename T>
  TypedAttrChecker<T>& AddAttr(const std::string& name,
                               const std::string& comment,
                               bool generated = false) {
    auto attr = proto_->mutable_attrs()->Add();
    *attr->mutable_name() = name;
    *attr->mutable_comment() = comment;
    attr->set_generated(generated);
    AttrTypeHelper::SetAttrType<T>(attr);
    return op_checker_->AddAttrChecker<T>(name);
  }

  void AddComment(const std::string& comment) {
    *(proto_->mutable_comment()) = comment;
  }

 private:
  void SetHasMultiple(const std::string& in_out, bool* flag) {
    if (!*flag) {
      AddAttr<std::vector<int>>(in_out + "_format",
                                "The multiple index of " + in_out +
                                    "\n"
                                    R"DOC(
This attribute is used by Paddle core framework. Paddle's Op support each input
or output could be a list of variable. This attribute is used to show how that
list organized.

e.g.
  input = ["a", "b", "c", "d", "e", "f"]
  input_format = [0, 4, 5, 6]

means
  The number of all input variables this op is six, and they are segmented into
  three inputs.

  The first input is input[0:4], second is input[4:5], third is input[5:6].
)DOC",
                                /*generated*/ true);
      *flag = true;
    }
  }

  void SetHasMultipleInput() { SetHasMultiple("input", &has_multiple_input_); }
  void SetHasMultipleOutput() {
    SetHasMultiple("output", &has_multiple_output_);
  }

  void SetHasTemporaryOutput() {
    if (!has_temporary_output_) {
      AddAttr<std::vector<int>>("temporary_index",
                                R"DOC(The temporary index of output.

Not all output of Paddle Op is used by user. For faster computation, each op
could output some its internal state to other op, other op could take that
output to make compute faster.

Add a mark to which output is temporary is helpful for future optimization.
)DOC",
                                /*generated*/ true)
          .SetDefault(std::vector<int>());
      has_temporary_output_ = true;
    }
  }

  void CheckNoDuplicatedInOutAttrs() {
    std::unordered_set<std::string> names;
    auto checker = [&](const std::string& name) {
      PADDLE_ENFORCE(!names.count(name), "[%s] is duplicated", name);
      names.insert(name);
    };
    for (auto& attr : proto_->attrs()) {
      checker(attr.name());
    }
    for (auto& input : proto_->inputs()) {
      checker(input.name());
    }
    for (auto& output : proto_->outputs()) {
      checker(output.name());
    }
  }

  OpProto* proto_;
  OpAttrChecker* op_checker_;
  bool validated_{false};
  bool has_multiple_input_{false};
  bool has_multiple_output_{false};
  bool has_temporary_output_{false};
};

class OpRegistry {
  using OpCreator = std::function<OperatorBase*()>;
  using VarIndexMap = std::unordered_map<std::string, int>;
  using VarNameList = std::vector<std::string>;

 public:
  template <typename OpType, typename ProtoMakerType>
  static void RegisterOp(const std::string& op_type) {
    creators()[op_type] = [] { return new OpType; };
    OpAttrChecker& op_checker = op_checkers()[op_type];
    OpProto& op_proto = protos()[op_type];
    auto maker = ProtoMakerType(&op_proto, &op_checker);
    maker.Validate();
    *op_proto.mutable_type() = op_type;
    PADDLE_ENFORCE(
        op_proto.IsInitialized(),
        "Fail to initialize %s's OpProto, because %s is not initialized",
        op_type, op_proto.InitializationErrorString());

    VarIndexMaps()[op_type].reset(new VarIndexMap());
    auto& varmap = *VarIndexMaps()[op_type];
    int idx = 0;
    for (auto& var : op_proto.inputs()) {
      varmap[var.name()] = idx++;
    }
    idx = 0;
    for (auto& var : op_proto.outputs()) {
      varmap[var.name()] = idx++;
    }
  }

  template <typename OpType>
  static void RegisterGradOp(const std::string& op_type) {
    grad_creators()[op_type] = [] { return new OpType; };
  }

  static OperatorPtr CreateOp(const std::string& type,
                              const VarNameList& inputs,
                              const VarNameList& outputs,
                              const AttributeMap& attrs) {
    auto op_create_it = creators().find(type);
    PADDLE_ENFORCE(op_create_it != creators().end(),
                   "Operator %s cannot be found.", type);

    auto op = op_create_it->second();
    op->type_ = type;
    op->inputs_ = inputs;
    op->outputs_ = outputs;

    op->attrs_ = attrs;
    op_checkers().at(type).Check(op->attrs_);

    GenerateTempVariableName(op);

    {
      auto var_index_it = VarIndexMaps().find(type);
      if (var_index_it != VarIndexMaps().end()) {
        op->in_out_idxs_ = var_index_it->second;
      }
    }

    op->Init();
    return OperatorPtr(op);
  }

  static OperatorPtr CreateOp(const OpDesc& op_desc) {
    std::vector<std::string> inputs;
    inputs.reserve((size_t)op_desc.inputs_size());
    std::copy(op_desc.inputs().begin(), op_desc.inputs().end(),
              std::back_inserter(inputs));

    std::vector<std::string> outputs;
    outputs.reserve((size_t)op_desc.outputs_size());
    std::copy(op_desc.outputs().begin(), op_desc.outputs().end(),
              std::back_inserter(outputs));

    AttributeMap attrs;
    for (auto& attr : op_desc.attrs()) {
      attrs[attr.name()] = AttrTypeHelper::GetAttrValue(attr);
    }

    return CreateOp(op_desc.type(), inputs, outputs, attrs);
  }

  static OperatorPtr CreateGradOp(OperatorPtr op) {
    OperatorPtr grad_op(grad_creators().at(op->type_)());
    grad_op->type_ = op->type_;

    AssembleGradInOut(op, grad_op);
    GenerateGradArgOffset(op, grad_op);
    GenerateGradAttr(op, grad_op);

    grad_op->Init();
    return grad_op;
  }

  static std::unordered_map<std::string, OpProto>& protos() {
    static std::unordered_map<std::string, OpProto> protos_;
    return protos_;
  };

 private:
  static std::unordered_map<std::string, std::shared_ptr<VarIndexMap>>&
  VarIndexMaps() {
    static std::unordered_map<std::string, std::shared_ptr<VarIndexMap>> maps_;
    return maps_;
  }

  static std::unordered_map<std::string, OpCreator>& creators() {
    static std::unordered_map<std::string, OpCreator> creators_;
    return creators_;
  }

  static std::unordered_map<std::string, OpAttrChecker>& op_checkers() {
    static std::unordered_map<std::string, OpAttrChecker> op_checkers_;
    return op_checkers_;
  };

  static std::unordered_map<std::string, OpCreator>& grad_creators() {
    static std::unordered_map<std::string, OpCreator> grad_creators_;
    return grad_creators_;
  }

  static void GenerateTempVariableName(OperatorBase* op) {
    static std::atomic<size_t> gUniqId(0UL);
    for (auto& outname : op->outputs_) {
      if (outname == OperatorBase::TMP_VAR_NAME()) {
        outname += op->type_;
        outname += "@";
        outname += std::to_string(gUniqId.fetch_add(1));
      }
    }
  }

  static void AssembleGradInOut(OperatorPtr op, OperatorPtr grad_op) {
    size_t in_sz = op->inputs_.size() + op->outputs_.size() * 2;
    grad_op->inputs_.reserve(in_sz);
    size_t out_sz = op->inputs_.size();
    grad_op->outputs_.reserve(out_sz);
    // copy op->inputs_ to grad_op->inputs_
    std::copy(op->inputs_.begin(), op->inputs_.end(),
              std::back_inserter(grad_op->inputs_));
    // copy op->outputs_ to grad_op->inputs_
    std::copy(op->outputs_.begin(), op->outputs_.end(),
              std::back_inserter(grad_op->inputs_));
    // add gradients of op->outputs_ to grad_op->inputs_
    for (const std::string& name : op->outputs_) {
      grad_op->inputs_.emplace_back(name + OperatorBase::GRAD_VAR_SUFFIX());
    }
    // add gradients of op->inputs_ to grad_op->outputs_
    for (const std::string& name : op->inputs_) {
      grad_op->outputs_.emplace_back(name + OperatorBase::GRAD_VAR_SUFFIX());
    }
  }

  static void GenerateGradArgOffset(OperatorPtr op, OperatorPtr grad_op) {
    VarIndexMap* grad_varmap = new VarIndexMap();
    const OpProto& op_proto = protos()[op->type_];
    int idx = 0;
    // offset of op's inputs
    for (const auto& var : op_proto.inputs()) {
      (*grad_varmap)[var.name()] = idx++;
    }
    // offset of op's outputs
    for (const auto& var : op_proto.outputs()) {
      (*grad_varmap)[var.name()] = idx++;
    }
    // offset of gradients of op's output
    for (const auto& var : op_proto.outputs()) {
      (*grad_varmap)[var.name() + OperatorBase::GRAD_VAR_SUFFIX()] = idx++;
    }
    idx = 0;
    // offset of gradients of op's input
    for (const auto& var : op_proto.inputs()) {
      (*grad_varmap)[var.name() + OperatorBase::GRAD_VAR_SUFFIX()] = idx++;
    }
    grad_op->in_out_idxs_.reset(grad_varmap);
  }

  static void GenerateGradAttr(OperatorPtr op, OperatorPtr grad_op) {
    const OpProto& op_proto = protos()[op->type_];
    grad_op->attrs_ = op->attrs_;
    grad_op->attrs_.erase("input_format");
    grad_op->attrs_.erase("output_format");
    bool has_in_format = op->attrs_.count("input_format");
    bool has_out_format = op->attrs_.count("output_format");
    // grad_op's inputs_ contains op's inputs_, outputs_ and gradients of
    // outpus_. So grad_op's input_format is necessary when op has
    // either input_format or output_format.
    if (has_in_format || has_out_format) {
      std::vector<int> old_in_format;
      std::vector<int> old_out_format;
      has_in_format
          ? old_in_format = op->GetAttr<std::vector<int>>("input_format")
          : old_in_format = std::vector<int>(op_proto.inputs_size()),
            std::iota(old_in_format.begin(), old_in_format.end(), 0);
      has_out_format
          ? old_out_format = op->GetAttr<std::vector<int>>("output_format")
          : old_out_format = std::vector<int>(op_proto.outputs_size()),
            std::iota(old_out_format.begin(), old_out_format.end(), 0);

      std::vector<int> in_format;
      in_format.reserve(old_in_format.size() + old_out_format.size() * 2);
      int base = 0;
      for (const int& idx : old_in_format) {
        in_format.emplace_back(idx + base);
      }
      base += op->inputs_.size();
      for (const int& idx : old_out_format) {
        in_format.emplace_back(idx + base);
      }
      base += op->outputs_.size();
      for (const int& idx : old_in_format) {
        in_format.emplace_back(idx + base);
      }
      grad_op->attrs_["input_format"] = in_format;
      // grad_op's outputs_ contains gradients of op's inputs_. So grad_op's
      // output_format is necessary only when op has input_format.
      if (has_in_format) {
        std::vector<int> out_format;
        out_format.reserve(op_proto.inputs_size());
        std::copy(old_in_format.begin(), old_in_format.end(),
                  std::back_inserter(out_format));
        grad_op->attrs_["output_format"] = out_format;
      }
    }
  }
};

template <typename OpType, typename ProtoMakerType>
class OpRegisterHelper {
 public:
  OpRegisterHelper(const char* op_type) {
    OpRegistry::RegisterOp<OpType, ProtoMakerType>(op_type);
  }
};

template <typename OpType>
class GradOpRegisterHelper {
 public:
  GradOpRegisterHelper(const char* op_type) {
    OpRegistry::RegisterGradOp<OpType>(op_type);
  }
};

/**
 * check if MACRO is used in GLOBAL NAMESPACE.
 */
#define STATIC_ASSERT_GLOBAL_NAMESPACE(uniq_name, msg)                        \
  struct __test_global_namespace_##uniq_name##__ {};                          \
  static_assert(std::is_same<::__test_global_namespace_##uniq_name##__,       \
                             __test_global_namespace_##uniq_name##__>::value, \
                msg)

/**
 * Macro to Register Operator.
 */
#define REGISTER_OP(__op_type, __op_class, __op_maker_class)                 \
  STATIC_ASSERT_GLOBAL_NAMESPACE(__reg_op__##__op_type,                      \
                                 "REGISTER_OP must be in global namespace"); \
  static ::paddle::framework::OpRegisterHelper<__op_class, __op_maker_class> \
      __op_register_##__op_type##__(#__op_type);                             \
  int __op_register_##__op_type##_handle__() { return 0; }

/**
 * Macro to Register Gradient Operator.
 */
#define REGISTER_GRADIENT_OP(__op_type, __op_class)            \
  STATIC_ASSERT_GLOBAL_NAMESPACE(                              \
      __reg_op__##__op_type,                                   \
      "REGISTER_GRADIENT_OP must be in global namespace");     \
  static ::paddle::framework::GradOpRegisterHelper<__op_class> \
      __op_register_##__op_type##__(#__op_type);               \
  int __op_register_##__op_type##_handle__() { return 0; }

/**
 * Macro to Register OperatorKernel.
 */
#define REGISTER_OP_KERNEL(type, DEVICE_TYPE, PlaceType, ...)             \
  STATIC_ASSERT_GLOBAL_NAMESPACE(                                         \
      __reg_op_kernel_##type##_##DEVICE_TYPE##__,                         \
      "REGISTER_OP_KERNEL must be in global namespace");                  \
  struct __op_kernel_register__##type##__ {                               \
    __op_kernel_register__##type##__() {                                  \
      ::paddle::framework::OperatorWithKernel::OpKernelKey key;           \
      key.place_ = PlaceType();                                           \
      ::paddle::framework::OperatorWithKernel::AllOpKernels()[#type][key] \
          .reset(new __VA_ARGS__());                                      \
    }                                                                     \
  };                                                                      \
  static __op_kernel_register__##type##__ __reg_kernel_##type##__;        \
  int __op_kernel_register_##type##_handle_##DEVICE_TYPE##__() { return 0; }

// (type, KernelType)
#define REGISTER_OP_GPU_KERNEL(type, ...) \
  REGISTER_OP_KERNEL(type, GPU, ::paddle::platform::GPUPlace, __VA_ARGS__)

// (type, KernelType)
#define REGISTER_OP_CPU_KERNEL(type, ...) \
  REGISTER_OP_KERNEL(type, CPU, ::paddle::platform::CPUPlace, __VA_ARGS__)

/**
 * Macro to mark what Operator and Kernel we will use and tell the compiler to
 * link them into target.
 */
#define USE_OP_WITHOUT_KERNEL(op_type)                      \
  STATIC_ASSERT_GLOBAL_NAMESPACE(                           \
      __use_op_without_kernel_##op_type,                    \
      "USE_OP_WITHOUT_KERNEL must be in global namespace"); \
  extern int __op_register_##op_type##_handle__();          \
  static int __use_op_ptr_##op_type##_without_kernel__      \
      __attribute__((unused)) = __op_register_##op_type##_handle__()

#define USE_OP_KERNEL(op_type, DEVICE_TYPE)                               \
  STATIC_ASSERT_GLOBAL_NAMESPACE(                                         \
      __use_op_kernel_##op_type##_##DEVICE_TYPE##__,                      \
      "USE_OP_KERNEL must be in global namespace");                       \
  extern int __op_kernel_register_##op_type##_handle_##DEVICE_TYPE##__(); \
  static int __use_op_ptr_##op_type##_##DEVICE_TYPE##_kernel__            \
      __attribute__((unused)) =                                           \
          __op_kernel_register_##op_type##_handle_##DEVICE_TYPE##__()

// use Operator with only cpu kernel.
#define USE_OP_CPU(op_type)       \
  USE_OP_WITHOUT_KERNEL(op_type); \
  USE_OP_KERNEL(op_type, CPU)

#ifdef PADDLE_ONLY_CPU
#define USE_OP(op_type) USE_OP_CPU(op_type)
#else
#define USE_OP(op_type) \
  USE_OP_CPU(op_type);  \
  USE_OP_KERNEL(op_type, GPU)
#endif

}  // namespace framework
}  // namespace paddle
