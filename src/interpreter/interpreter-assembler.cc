// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/interpreter/interpreter-assembler.h"

#include <ostream>

#include "src/code-factory.h"
#include "src/frames.h"
#include "src/interface-descriptors.h"
#include "src/interpreter/bytecodes.h"
#include "src/machine-type.h"
#include "src/macro-assembler.h"
#include "src/zone.h"

namespace v8 {
namespace internal {
namespace interpreter {

using compiler::Node;

InterpreterAssembler::InterpreterAssembler(Isolate* isolate, Zone* zone,
                                           Bytecode bytecode)
    : compiler::CodeStubAssembler(
          isolate, zone, InterpreterDispatchDescriptor(isolate),
          Code::ComputeFlags(Code::STUB), Bytecodes::ToString(bytecode), 0),
      bytecode_(bytecode),
      accumulator_(this, MachineRepresentation::kTagged),
      context_(this, MachineRepresentation::kTagged),
      dispatch_table_(this, MachineType::PointerRepresentation()),
      disable_stack_check_across_call_(false),
      stack_pointer_before_call_(nullptr) {
  accumulator_.Bind(
      Parameter(InterpreterDispatchDescriptor::kAccumulatorParameter));
  context_.Bind(Parameter(InterpreterDispatchDescriptor::kContextParameter));
  dispatch_table_.Bind(
      Parameter(InterpreterDispatchDescriptor::kDispatchTableParameter));
  if (FLAG_trace_ignition) {
    TraceBytecode(Runtime::kInterpreterTraceBytecodeEntry);
  }
}

InterpreterAssembler::~InterpreterAssembler() {}

Node* InterpreterAssembler::GetAccumulator() { return accumulator_.value(); }

void InterpreterAssembler::SetAccumulator(Node* value) {
  accumulator_.Bind(value);
}

Node* InterpreterAssembler::GetContext() { return context_.value(); }

void InterpreterAssembler::SetContext(Node* value) {
  StoreRegister(value, Register::current_context());
  context_.Bind(value);
}

Node* InterpreterAssembler::BytecodeOffset() {
  return Parameter(InterpreterDispatchDescriptor::kBytecodeOffsetParameter);
}

Node* InterpreterAssembler::RegisterFileRawPointer() {
  return Parameter(InterpreterDispatchDescriptor::kRegisterFileParameter);
}

Node* InterpreterAssembler::BytecodeArrayTaggedPointer() {
  return Parameter(InterpreterDispatchDescriptor::kBytecodeArrayParameter);
}

Node* InterpreterAssembler::DispatchTableRawPointer() {
  return dispatch_table_.value();
}

Node* InterpreterAssembler::RegisterLocation(Node* reg_index) {
  return IntPtrAdd(RegisterFileRawPointer(), RegisterFrameOffset(reg_index));
}

Node* InterpreterAssembler::LoadRegister(int offset) {
  return Load(MachineType::AnyTagged(), RegisterFileRawPointer(),
              Int32Constant(offset));
}

Node* InterpreterAssembler::LoadRegister(Register reg) {
  return LoadRegister(reg.ToOperand() << kPointerSizeLog2);
}

Node* InterpreterAssembler::RegisterFrameOffset(Node* index) {
  return WordShl(index, kPointerSizeLog2);
}

Node* InterpreterAssembler::LoadRegister(Node* reg_index) {
  return Load(MachineType::AnyTagged(), RegisterFileRawPointer(),
              RegisterFrameOffset(reg_index));
}

Node* InterpreterAssembler::StoreRegister(Node* value, int offset) {
  return StoreNoWriteBarrier(MachineRepresentation::kTagged,
                             RegisterFileRawPointer(), Int32Constant(offset),
                             value);
}

Node* InterpreterAssembler::StoreRegister(Node* value, Register reg) {
  return StoreRegister(value, reg.ToOperand() << kPointerSizeLog2);
}

Node* InterpreterAssembler::StoreRegister(Node* value, Node* reg_index) {
  return StoreNoWriteBarrier(MachineRepresentation::kTagged,
                             RegisterFileRawPointer(),
                             RegisterFrameOffset(reg_index), value);
}

Node* InterpreterAssembler::NextRegister(Node* reg_index) {
  // Register indexes are negative, so the next index is minus one.
  return IntPtrAdd(reg_index, Int32Constant(-1));
}

Node* InterpreterAssembler::BytecodeOperand(int operand_index) {
  DCHECK_LT(operand_index, Bytecodes::NumberOfOperands(bytecode_));
  DCHECK_EQ(OperandSize::kByte,
            Bytecodes::GetOperandSize(bytecode_, operand_index));
  return Load(
      MachineType::Uint8(), BytecodeArrayTaggedPointer(),
      IntPtrAdd(BytecodeOffset(), Int32Constant(Bytecodes::GetOperandOffset(
                                      bytecode_, operand_index))));
}

Node* InterpreterAssembler::BytecodeOperandSignExtended(int operand_index) {
  DCHECK_LT(operand_index, Bytecodes::NumberOfOperands(bytecode_));
  DCHECK_EQ(OperandSize::kByte,
            Bytecodes::GetOperandSize(bytecode_, operand_index));
  Node* load = Load(
      MachineType::Int8(), BytecodeArrayTaggedPointer(),
      IntPtrAdd(BytecodeOffset(), Int32Constant(Bytecodes::GetOperandOffset(
                                      bytecode_, operand_index))));
  // Ensure that we sign extend to full pointer size
  if (kPointerSize == 8) {
    load = ChangeInt32ToInt64(load);
  }
  return load;
}

Node* InterpreterAssembler::BytecodeOperandShort(int operand_index) {
  DCHECK_LT(operand_index, Bytecodes::NumberOfOperands(bytecode_));
  DCHECK_EQ(OperandSize::kShort,
            Bytecodes::GetOperandSize(bytecode_, operand_index));
  if (TargetSupportsUnalignedAccess()) {
    return Load(
        MachineType::Uint16(), BytecodeArrayTaggedPointer(),
        IntPtrAdd(BytecodeOffset(), Int32Constant(Bytecodes::GetOperandOffset(
                                        bytecode_, operand_index))));
  } else {
    int offset = Bytecodes::GetOperandOffset(bytecode_, operand_index);
    Node* first_byte = Load(MachineType::Uint8(), BytecodeArrayTaggedPointer(),
                            IntPtrAdd(BytecodeOffset(), Int32Constant(offset)));
    Node* second_byte =
        Load(MachineType::Uint8(), BytecodeArrayTaggedPointer(),
             IntPtrAdd(BytecodeOffset(), Int32Constant(offset + 1)));
#if V8_TARGET_LITTLE_ENDIAN
    return WordOr(WordShl(second_byte, kBitsPerByte), first_byte);
#elif V8_TARGET_BIG_ENDIAN
    return WordOr(WordShl(first_byte, kBitsPerByte), second_byte);
#else
#error "Unknown Architecture"
#endif
  }
}

Node* InterpreterAssembler::BytecodeOperandShortSignExtended(
    int operand_index) {
  DCHECK_LT(operand_index, Bytecodes::NumberOfOperands(bytecode_));
  DCHECK_EQ(OperandSize::kShort,
            Bytecodes::GetOperandSize(bytecode_, operand_index));
  int operand_offset = Bytecodes::GetOperandOffset(bytecode_, operand_index);
  Node* load;
  if (TargetSupportsUnalignedAccess()) {
    load = Load(MachineType::Int16(), BytecodeArrayTaggedPointer(),
                IntPtrAdd(BytecodeOffset(), Int32Constant(operand_offset)));
  } else {
#if V8_TARGET_LITTLE_ENDIAN
    Node* hi_byte_offset = Int32Constant(operand_offset + 1);
    Node* lo_byte_offset = Int32Constant(operand_offset);
#elif V8_TARGET_BIG_ENDIAN
    Node* hi_byte_offset = Int32Constant(operand_offset);
    Node* lo_byte_offset = Int32Constant(operand_offset + 1);
#else
#error "Unknown Architecture"
#endif
    Node* hi_byte = Load(MachineType::Int8(), BytecodeArrayTaggedPointer(),
                         IntPtrAdd(BytecodeOffset(), hi_byte_offset));
    Node* lo_byte = Load(MachineType::Uint8(), BytecodeArrayTaggedPointer(),
                         IntPtrAdd(BytecodeOffset(), lo_byte_offset));
    hi_byte = Word32Shl(hi_byte, Int32Constant(kBitsPerByte));
    load = Word32Or(hi_byte, lo_byte);
  }

  // Ensure that we sign extend to full pointer size
  if (kPointerSize == 8) {
    load = ChangeInt32ToInt64(load);
  }
  return load;
}

Node* InterpreterAssembler::BytecodeOperandCount(int operand_index) {
  switch (Bytecodes::GetOperandSize(bytecode_, operand_index)) {
    case OperandSize::kByte:
      DCHECK_EQ(OperandType::kRegCount8,
                Bytecodes::GetOperandType(bytecode_, operand_index));
      return BytecodeOperand(operand_index);
    case OperandSize::kShort:
      DCHECK_EQ(OperandType::kRegCount16,
                Bytecodes::GetOperandType(bytecode_, operand_index));
      return BytecodeOperandShort(operand_index);
    case OperandSize::kNone:
      UNREACHABLE();
  }
  return nullptr;
}

Node* InterpreterAssembler::BytecodeOperandImm(int operand_index) {
  DCHECK_EQ(OperandType::kImm8,
            Bytecodes::GetOperandType(bytecode_, operand_index));
  return BytecodeOperandSignExtended(operand_index);
}

Node* InterpreterAssembler::BytecodeOperandIdx(int operand_index) {
  switch (Bytecodes::GetOperandSize(bytecode_, operand_index)) {
    case OperandSize::kByte:
      DCHECK_EQ(OperandType::kIdx8,
                Bytecodes::GetOperandType(bytecode_, operand_index));
      return BytecodeOperand(operand_index);
    case OperandSize::kShort:
      DCHECK_EQ(OperandType::kIdx16,
                Bytecodes::GetOperandType(bytecode_, operand_index));
      return BytecodeOperandShort(operand_index);
    case OperandSize::kNone:
      UNREACHABLE();
  }
  return nullptr;
}

Node* InterpreterAssembler::BytecodeOperandReg(int operand_index) {
  OperandType operand_type =
      Bytecodes::GetOperandType(bytecode_, operand_index);
  if (Bytecodes::IsRegisterOperandType(operand_type)) {
    OperandSize operand_size = Bytecodes::SizeOfOperand(operand_type);
    if (operand_size == OperandSize::kByte) {
      return BytecodeOperandSignExtended(operand_index);
    } else if (operand_size == OperandSize::kShort) {
      return BytecodeOperandShortSignExtended(operand_index);
    }
  }
  UNREACHABLE();
  return nullptr;
}

Node* InterpreterAssembler::LoadConstantPoolEntry(Node* index) {
  Node* constant_pool = LoadObjectField(BytecodeArrayTaggedPointer(),
                                        BytecodeArray::kConstantPoolOffset);
  Node* entry_offset =
      IntPtrAdd(IntPtrConstant(FixedArray::kHeaderSize - kHeapObjectTag),
                WordShl(index, kPointerSizeLog2));
  return Load(MachineType::AnyTagged(), constant_pool, entry_offset);
}

Node* InterpreterAssembler::LoadFixedArrayElement(Node* fixed_array,
                                                  int index) {
  Node* entry_offset =
      IntPtrAdd(IntPtrConstant(FixedArray::kHeaderSize - kHeapObjectTag),
                WordShl(Int32Constant(index), kPointerSizeLog2));
  return Load(MachineType::AnyTagged(), fixed_array, entry_offset);
}

Node* InterpreterAssembler::LoadObjectField(Node* object, int offset) {
  return Load(MachineType::AnyTagged(), object,
              IntPtrConstant(offset - kHeapObjectTag));
}

Node* InterpreterAssembler::LoadContextSlot(Node* context, int slot_index) {
  return Load(MachineType::AnyTagged(), context,
              IntPtrConstant(Context::SlotOffset(slot_index)));
}

Node* InterpreterAssembler::LoadContextSlot(Node* context, Node* slot_index) {
  Node* offset =
      IntPtrAdd(WordShl(slot_index, kPointerSizeLog2),
                Int32Constant(Context::kHeaderSize - kHeapObjectTag));
  return Load(MachineType::AnyTagged(), context, offset);
}

Node* InterpreterAssembler::StoreContextSlot(Node* context, Node* slot_index,
                                             Node* value) {
  Node* offset =
      IntPtrAdd(WordShl(slot_index, kPointerSizeLog2),
                Int32Constant(Context::kHeaderSize - kHeapObjectTag));
  return Store(MachineRepresentation::kTagged, context, offset, value);
}

Node* InterpreterAssembler::LoadTypeFeedbackVector() {
  Node* function = Load(
      MachineType::AnyTagged(), RegisterFileRawPointer(),
      IntPtrConstant(InterpreterFrameConstants::kFunctionFromRegisterPointer));
  Node* shared_info =
      LoadObjectField(function, JSFunction::kSharedFunctionInfoOffset);
  Node* vector =
      LoadObjectField(shared_info, SharedFunctionInfo::kFeedbackVectorOffset);
  return vector;
}

void InterpreterAssembler::CallPrologue() {
  StoreRegister(SmiTag(BytecodeOffset()),
                InterpreterFrameConstants::kBytecodeOffsetFromRegisterPointer);
  StoreRegister(DispatchTableRawPointer(),
                InterpreterFrameConstants::kDispatchTableFromRegisterPointer);

  if (FLAG_debug_code && !disable_stack_check_across_call_) {
    DCHECK(stack_pointer_before_call_ == nullptr);
    stack_pointer_before_call_ = LoadStackPointer();
  }
}

void InterpreterAssembler::CallEpilogue() {
  if (FLAG_debug_code && !disable_stack_check_across_call_) {
    Node* stack_pointer_after_call = LoadStackPointer();
    Node* stack_pointer_before_call = stack_pointer_before_call_;
    stack_pointer_before_call_ = nullptr;
    AbortIfWordNotEqual(stack_pointer_before_call, stack_pointer_after_call,
                        kUnexpectedStackPointer);
  }

  // Restore dispatch table from stack frame in case the debugger has swapped us
  // to the debugger dispatch table.
  dispatch_table_.Bind(LoadRegister(
      InterpreterFrameConstants::kDispatchTableFromRegisterPointer));
}

Node* InterpreterAssembler::CallJS(Node* function, Node* context,
                                   Node* first_arg, Node* arg_count) {
  Callable callable = CodeFactory::InterpreterPushArgsAndCall(isolate());
  Node* code_target = HeapConstant(callable.code());
  return CallStub(callable.descriptor(), code_target, context, arg_count,
                  first_arg, function);
}

Node* InterpreterAssembler::CallConstruct(Node* constructor, Node* context,
                                          Node* new_target, Node* first_arg,
                                          Node* arg_count) {
  Callable callable = CodeFactory::InterpreterPushArgsAndConstruct(isolate());
  Node* code_target = HeapConstant(callable.code());
  return CallStub(callable.descriptor(), code_target, context, arg_count,
                  new_target, constructor, first_arg);
}

Node* InterpreterAssembler::CallRuntimeN(Node* function_id, Node* context,
                                         Node* first_arg, Node* arg_count,
                                         int result_size) {
  Callable callable = CodeFactory::InterpreterCEntry(isolate(), result_size);
  Node* code_target = HeapConstant(callable.code());

  // Get the function entry from the function id.
  Node* function_table = ExternalConstant(
      ExternalReference::runtime_function_table_address(isolate()));
  Node* function_offset =
      Int32Mul(function_id, Int32Constant(sizeof(Runtime::Function)));
  Node* function = IntPtrAdd(function_table, function_offset);
  Node* function_entry =
      Load(MachineType::Pointer(), function,
           Int32Constant(offsetof(Runtime::Function, entry)));

  return CallStub(callable.descriptor(), code_target, context, arg_count,
                  first_arg, function_entry, result_size);
}

Node* InterpreterAssembler::Advance(int delta) {
  return IntPtrAdd(BytecodeOffset(), Int32Constant(delta));
}

Node* InterpreterAssembler::Advance(Node* delta) {
  return IntPtrAdd(BytecodeOffset(), delta);
}

void InterpreterAssembler::Jump(Node* delta) { DispatchTo(Advance(delta)); }

void InterpreterAssembler::JumpConditional(Node* condition, Node* delta) {
  CodeStubAssembler::Label match(this);
  CodeStubAssembler::Label no_match(this);

  Branch(condition, &match, &no_match);
  Bind(&match);
  DispatchTo(Advance(delta));
  Bind(&no_match);
  Dispatch();
}

void InterpreterAssembler::JumpIfWordEqual(Node* lhs, Node* rhs, Node* delta) {
  JumpConditional(WordEqual(lhs, rhs), delta);
}

void InterpreterAssembler::JumpIfWordNotEqual(Node* lhs, Node* rhs,
                                              Node* delta) {
  JumpConditional(WordNotEqual(lhs, rhs), delta);
}

void InterpreterAssembler::Dispatch() {
  DispatchTo(Advance(Bytecodes::Size(bytecode_)));
}

void InterpreterAssembler::DispatchTo(Node* new_bytecode_offset) {
  if (FLAG_trace_ignition) {
    TraceBytecode(Runtime::kInterpreterTraceBytecodeExit);
  }
  Node* target_bytecode = Load(
      MachineType::Uint8(), BytecodeArrayTaggedPointer(), new_bytecode_offset);

  // TODO(rmcilroy): Create a code target dispatch table to avoid conversion
  // from code object on every dispatch.
  Node* target_code_object =
      Load(MachineType::Pointer(), DispatchTableRawPointer(),
           Word32Shl(target_bytecode, Int32Constant(kPointerSizeLog2)));

  InterpreterDispatchDescriptor descriptor(isolate());
  Node* args[] = {GetAccumulator(),          RegisterFileRawPointer(),
                  new_bytecode_offset,       BytecodeArrayTaggedPointer(),
                  DispatchTableRawPointer(), GetContext()};
  TailCall(descriptor, target_code_object, args, 0);
}

void InterpreterAssembler::InterpreterReturn() {
  if (FLAG_trace_ignition) {
    TraceBytecode(Runtime::kInterpreterTraceBytecodeExit);
  }
  InterpreterDispatchDescriptor descriptor(isolate());
  Node* exit_trampoline_code_object =
      HeapConstant(isolate()->builtins()->InterpreterExitTrampoline());
  Node* args[] = {GetAccumulator(),          RegisterFileRawPointer(),
                  BytecodeOffset(),          BytecodeArrayTaggedPointer(),
                  DispatchTableRawPointer(), GetContext()};
  TailCall(descriptor, exit_trampoline_code_object, args, 0);
}

void InterpreterAssembler::StackCheck() {
  CodeStubAssembler::Label end(this);
  CodeStubAssembler::Label ok(this);
  CodeStubAssembler::Label stack_guard(this);

  Node* sp = LoadStackPointer();
  Node* stack_limit = Load(
      MachineType::Pointer(),
      ExternalConstant(ExternalReference::address_of_stack_limit(isolate())));
  Node* condition = UintPtrGreaterThanOrEqual(sp, stack_limit);
  Branch(condition, &ok, &stack_guard);
  Bind(&stack_guard);
  CallRuntime(Runtime::kStackGuard, GetContext());
  Goto(&end);
  Bind(&ok);
  Goto(&end);
  Bind(&end);
}

void InterpreterAssembler::Abort(BailoutReason bailout_reason) {
  disable_stack_check_across_call_ = true;
  Node* abort_id = SmiTag(Int32Constant(bailout_reason));
  Node* ret_value = CallRuntime(Runtime::kAbort, GetContext(), abort_id);
  disable_stack_check_across_call_ = false;
  // Unreached, but keeps turbofan happy.
  Return(ret_value);
}

void InterpreterAssembler::AbortIfWordNotEqual(Node* lhs, Node* rhs,
                                               BailoutReason bailout_reason) {
  CodeStubAssembler::Label match(this);
  CodeStubAssembler::Label no_match(this);

  Node* condition = WordEqual(lhs, rhs);
  Branch(condition, &match, &no_match);
  Bind(&no_match);
  Abort(bailout_reason);
  Bind(&match);
}

void InterpreterAssembler::TraceBytecode(Runtime::FunctionId function_id) {
  CallRuntime(function_id, GetContext(), BytecodeArrayTaggedPointer(),
              SmiTag(BytecodeOffset()), GetAccumulator());
}

// static
bool InterpreterAssembler::TargetSupportsUnalignedAccess() {
#if V8_TARGET_ARCH_MIPS || V8_TARGET_ARCH_MIPS64
  return false;
#elif V8_TARGET_ARCH_ARM || V8_TARGET_ARCH_ARM64 || V8_TARGET_ARCH_PPC
  return CpuFeatures::IsSupported(UNALIGNED_ACCESSES);
#elif V8_TARGET_ARCH_IA32 || V8_TARGET_ARCH_X64 || V8_TARGET_ARCH_X87
  return true;
#else
#error "Unknown Architecture"
#endif
}

}  // namespace interpreter
}  // namespace internal
}  // namespace v8
