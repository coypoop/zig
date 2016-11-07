#include "analyze.hpp"
#include "error.hpp"
#include "eval.hpp"
#include "ir.hpp"
#include "ir_print.hpp"

struct IrExecContext {
    ConstExprValue *mem_slot_list;
    size_t mem_slot_count;
};

struct IrBuilder {
    CodeGen *codegen;
    IrExecutable *exec;
    IrBasicBlock *current_basic_block;
    ZigList<IrBasicBlock *> break_block_stack;
    ZigList<IrBasicBlock *> continue_block_stack;
};

struct IrAnalyze {
    CodeGen *codegen;
    IrBuilder old_irb;
    IrBuilder new_irb;
    IrExecContext exec_context;
    ZigList<IrBasicBlock *> old_bb_queue;
    size_t block_queue_index;
    size_t instruction_index;
    TypeTableEntry *explicit_return_type;
    ZigList<IrInstruction *> implicit_return_type_list;
    IrBasicBlock *const_predecessor_bb;
};

static IrInstruction *ir_gen_node(IrBuilder *irb, AstNode *node, BlockContext *scope);
static IrInstruction *ir_gen_node_extra(IrBuilder *irb, AstNode *node, BlockContext *block_context,
        LValPurpose lval);
static TypeTableEntry *ir_analyze_instruction(IrAnalyze *ira, IrInstruction *instruction);

static void ir_instruction_append(IrBasicBlock *basic_block, IrInstruction *instruction) {
    assert(basic_block);
    assert(instruction);
    basic_block->instruction_list.append(instruction);
}

static size_t exec_next_debug_id(IrExecutable *exec) {
    size_t result = exec->next_debug_id;
    exec->next_debug_id += 1;
    return result;
}

static size_t exec_next_mem_slot(IrExecutable *exec) {
    size_t result = exec->mem_slot_count;
    exec->mem_slot_count += 1;
    return result;
}

static void ir_link_new_instruction(IrInstruction *new_instruction, IrInstruction *old_instruction) {
    new_instruction->other = old_instruction;
    old_instruction->other = new_instruction;
}

static void ir_link_new_bb(IrBasicBlock *new_bb, IrBasicBlock *old_bb) {
    new_bb->other = old_bb;
    old_bb->other = new_bb;
}

static void ir_ref_bb(IrBasicBlock *bb) {
    bb->ref_count += 1;
}

static void ir_ref_instruction(IrInstruction *instruction) {
    instruction->ref_count += 1;
}

static void ir_ref_var(VariableTableEntry *var) {
    var->ref_count += 1;
}

static IrBasicBlock *ir_build_basic_block(IrBuilder *irb, const char *name_hint) {
    IrBasicBlock *result = allocate<IrBasicBlock>(1);
    result->name_hint = name_hint;
    result->debug_id = exec_next_debug_id(irb->exec);
    irb->exec->basic_block_list.append(result);
    return result;
}

static IrBasicBlock *ir_build_bb_from(IrBuilder *irb, IrBasicBlock *other_bb) {
    IrBasicBlock *new_bb = ir_build_basic_block(irb, other_bb->name_hint);
    ir_link_new_bb(new_bb, other_bb);
    return new_bb;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionCondBr *) {
    return IrInstructionIdCondBr;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionBr *) {
    return IrInstructionIdBr;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionSwitchBr *) {
    return IrInstructionIdSwitchBr;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionPhi *) {
    return IrInstructionIdPhi;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionUnOp *) {
    return IrInstructionIdUnOp;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionBinOp *) {
    return IrInstructionIdBinOp;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionDeclVar *) {
    return IrInstructionIdDeclVar;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionLoadPtr *) {
    return IrInstructionIdLoadPtr;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionStorePtr *) {
    return IrInstructionIdStorePtr;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionFieldPtr *) {
    return IrInstructionIdFieldPtr;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionStructFieldPtr *) {
    return IrInstructionIdStructFieldPtr;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionReadField *) {
    return IrInstructionIdReadField;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionElemPtr *) {
    return IrInstructionIdElemPtr;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionVarPtr *) {
    return IrInstructionIdVarPtr;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionCall *) {
    return IrInstructionIdCall;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionConst *) {
    return IrInstructionIdConst;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionReturn *) {
    return IrInstructionIdReturn;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionCast *) {
    return IrInstructionIdCast;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionContainerInitList *) {
    return IrInstructionIdContainerInitList;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionContainerInitFields *) {
    return IrInstructionIdContainerInitFields;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionUnreachable *) {
    return IrInstructionIdUnreachable;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionTypeOf *) {
    return IrInstructionIdTypeOf;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionToPtrType *) {
    return IrInstructionIdToPtrType;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionPtrTypeChild *) {
    return IrInstructionIdPtrTypeChild;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionSetFnTest *) {
    return IrInstructionIdSetFnTest;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionArrayType *) {
    return IrInstructionIdArrayType;
}

static constexpr IrInstructionId ir_instruction_id(IrInstructionSliceType *) {
    return IrInstructionIdSliceType;
}

template<typename T>
static T *ir_create_instruction(IrExecutable *exec, AstNode *source_node) {
    T *special_instruction = allocate<T>(1);
    special_instruction->base.id = ir_instruction_id(special_instruction);
    special_instruction->base.source_node = source_node;
    special_instruction->base.debug_id = exec_next_debug_id(exec);
    return special_instruction;
}

template<typename T>
static T *ir_build_instruction(IrBuilder *irb, AstNode *source_node) {
    assert(source_node);
    T *special_instruction = ir_create_instruction<T>(irb->exec, source_node);
    ir_instruction_append(irb->current_basic_block, &special_instruction->base);
    return special_instruction;
}

static IrInstruction *ir_build_cast(IrBuilder *irb, AstNode *source_node, IrInstruction *dest_type,
        IrInstruction *value, CastOp cast_op)
{
    IrInstructionCast *cast_instruction = ir_build_instruction<IrInstructionCast>(irb, source_node);
    cast_instruction->dest_type = dest_type;
    cast_instruction->value = value;
    cast_instruction->cast_op = cast_op;

    ir_ref_instruction(dest_type);
    ir_ref_instruction(value);

    return &cast_instruction->base;
}

static IrInstruction *ir_build_cond_br(IrBuilder *irb, AstNode *source_node, IrInstruction *condition,
        IrBasicBlock *then_block, IrBasicBlock *else_block, bool is_inline)
{
    IrInstructionCondBr *cond_br_instruction = ir_build_instruction<IrInstructionCondBr>(irb, source_node);
    cond_br_instruction->base.type_entry = irb->codegen->builtin_types.entry_unreachable;
    cond_br_instruction->base.static_value.ok = true;
    cond_br_instruction->condition = condition;
    cond_br_instruction->then_block = then_block;
    cond_br_instruction->else_block = else_block;
    cond_br_instruction->is_inline = is_inline;

    ir_ref_instruction(condition);
    ir_ref_bb(then_block);
    ir_ref_bb(else_block);

    return &cond_br_instruction->base;
}

static IrInstruction *ir_build_cond_br_from(IrBuilder *irb, IrInstruction *old_instruction,
        IrInstruction *condition, IrBasicBlock *then_block, IrBasicBlock *else_block, bool is_inline)
{
    IrInstruction *new_instruction = ir_build_cond_br(irb, old_instruction->source_node,
            condition, then_block, else_block, is_inline);
    ir_link_new_instruction(new_instruction, old_instruction);
    return new_instruction;
}

static IrInstruction *ir_build_return(IrBuilder *irb, AstNode *source_node, IrInstruction *return_value) {
    IrInstructionReturn *return_instruction = ir_build_instruction<IrInstructionReturn>(irb, source_node);
    return_instruction->base.type_entry = irb->codegen->builtin_types.entry_unreachable;
    return_instruction->base.static_value.ok = true;
    return_instruction->value = return_value;

    ir_ref_instruction(return_value);

    return &return_instruction->base;
}

static IrInstruction *ir_build_return_from(IrBuilder *irb, IrInstruction *old_instruction,
        IrInstruction *return_value)
{
    IrInstruction *new_instruction = ir_build_return(irb, old_instruction->source_node, return_value);
    ir_link_new_instruction(new_instruction, old_instruction);
    return new_instruction;
}

static IrInstruction *ir_create_const(IrBuilder *irb, AstNode *source_node, TypeTableEntry *type_entry) {
    IrInstructionConst *const_instruction = ir_create_instruction<IrInstructionConst>(irb->exec, source_node);
    const_instruction->base.type_entry = type_entry;
    const_instruction->base.static_value.ok = true;
    return &const_instruction->base;
}

static IrInstruction *ir_build_const_void(IrBuilder *irb, AstNode *source_node) {
    IrInstructionConst *const_instruction = ir_build_instruction<IrInstructionConst>(irb, source_node);
    const_instruction->base.type_entry = irb->codegen->builtin_types.entry_void;
    const_instruction->base.static_value.ok = true;
    return &const_instruction->base;
}

static IrInstruction *ir_build_const_undefined(IrBuilder *irb, AstNode *source_node) {
    IrInstructionConst *const_instruction = ir_build_instruction<IrInstructionConst>(irb, source_node);
    const_instruction->base.static_value.ok = true;
    const_instruction->base.static_value.special = ConstValSpecialUndef;
    const_instruction->base.type_entry = irb->codegen->builtin_types.entry_undef;
    return &const_instruction->base;
}

static IrInstruction *ir_build_const_bignum(IrBuilder *irb, AstNode *source_node, BigNum *bignum) {
    IrInstructionConst *const_instruction = ir_build_instruction<IrInstructionConst>(irb, source_node);
    const_instruction->base.type_entry = (bignum->kind == BigNumKindInt) ?
        irb->codegen->builtin_types.entry_num_lit_int : irb->codegen->builtin_types.entry_num_lit_float;
    const_instruction->base.static_value.ok = true;
    const_instruction->base.static_value.data.x_bignum = *bignum;
    return &const_instruction->base;
}

static IrInstruction *ir_build_const_usize(IrBuilder *irb, AstNode *source_node, uint64_t value) {
    IrInstructionConst *const_instruction = ir_build_instruction<IrInstructionConst>(irb, source_node);
    const_instruction->base.type_entry = irb->codegen->builtin_types.entry_usize;
    const_instruction->base.static_value.ok = true;
    bignum_init_unsigned(&const_instruction->base.static_value.data.x_bignum, value);
    return &const_instruction->base;
}

static IrInstruction *ir_create_const_type(IrBuilder *irb, AstNode *source_node, TypeTableEntry *type_entry) {
    IrInstructionConst *const_instruction = ir_create_instruction<IrInstructionConst>(irb->exec, source_node);
    const_instruction->base.type_entry = irb->codegen->builtin_types.entry_type;
    const_instruction->base.static_value.ok = true;
    const_instruction->base.static_value.data.x_type = type_entry;
    return &const_instruction->base;
}

static IrInstruction *ir_build_const_type(IrBuilder *irb, AstNode *source_node, TypeTableEntry *type_entry) {
    IrInstruction *instruction = ir_create_const_type(irb, source_node, type_entry);
    ir_instruction_append(irb->current_basic_block, instruction);
    return instruction;
}

static IrInstruction *ir_build_const_fn(IrBuilder *irb, AstNode *source_node, FnTableEntry *fn_entry) {
    IrInstructionConst *const_instruction = ir_build_instruction<IrInstructionConst>(irb, source_node);
    const_instruction->base.type_entry = fn_entry->type_entry;
    const_instruction->base.static_value.ok = true;
    const_instruction->base.static_value.data.x_fn = fn_entry;
    return &const_instruction->base;
}

static IrInstruction *ir_build_const_generic_fn(IrBuilder *irb, AstNode *source_node, TypeTableEntry *fn_type) {
    IrInstructionConst *const_instruction = ir_build_instruction<IrInstructionConst>(irb, source_node);
    const_instruction->base.type_entry = fn_type;
    const_instruction->base.static_value.ok = true;
    const_instruction->base.static_value.data.x_type = fn_type;
    return &const_instruction->base;
}

static IrInstruction *ir_build_const_import(IrBuilder *irb, AstNode *source_node, ImportTableEntry *import) {
    IrInstructionConst *const_instruction = ir_build_instruction<IrInstructionConst>(irb, source_node);
    const_instruction->base.type_entry = irb->codegen->builtin_types.entry_namespace;
    const_instruction->base.static_value.ok = true;
    const_instruction->base.static_value.data.x_import = import;
    return &const_instruction->base;
}

static IrInstruction *ir_build_const_scope(IrBuilder *irb, AstNode *source_node, BlockContext *scope) {
    IrInstructionConst *const_instruction = ir_build_instruction<IrInstructionConst>(irb, source_node);
    const_instruction->base.type_entry = irb->codegen->builtin_types.entry_block;
    const_instruction->base.static_value.ok = true;
    const_instruction->base.static_value.data.x_block = scope;
    return &const_instruction->base;
}

static IrInstruction *ir_build_const_bool(IrBuilder *irb, AstNode *source_node, bool value) {
    IrInstructionConst *const_instruction = ir_build_instruction<IrInstructionConst>(irb, source_node);
    const_instruction->base.type_entry = irb->codegen->builtin_types.entry_bool;
    const_instruction->base.static_value.ok = true;
    const_instruction->base.static_value.data.x_bool = value;
    return &const_instruction->base;
}

static IrInstruction *ir_build_bin_op(IrBuilder *irb, AstNode *source_node, IrBinOp op_id,
        IrInstruction *op1, IrInstruction *op2)
{
    IrInstructionBinOp *bin_op_instruction = ir_build_instruction<IrInstructionBinOp>(irb, source_node);
    bin_op_instruction->op_id = op_id;
    bin_op_instruction->op1 = op1;
    bin_op_instruction->op2 = op2;

    ir_ref_instruction(op1);
    ir_ref_instruction(op2);

    return &bin_op_instruction->base;
}

static IrInstruction *ir_build_bin_op_from(IrBuilder *irb, IrInstruction *old_instruction, IrBinOp op_id,
        IrInstruction *op1, IrInstruction *op2)
{
    IrInstruction *new_instruction = ir_build_bin_op(irb, old_instruction->source_node, op_id, op1, op2);
    ir_link_new_instruction(new_instruction, old_instruction);
    return new_instruction;
}

static IrInstruction *ir_build_var_ptr(IrBuilder *irb, AstNode *source_node, VariableTableEntry *var) {
    IrInstructionVarPtr *instruction = ir_build_instruction<IrInstructionVarPtr>(irb, source_node);
    instruction->var = var;

    ir_ref_var(var);

    return &instruction->base;
}

static IrInstruction *ir_build_var_ptr_from(IrBuilder *irb, IrInstruction *old_instruction,
        VariableTableEntry *var)
{
    IrInstruction *new_instruction = ir_build_var_ptr(irb, old_instruction->source_node, var);
    ir_link_new_instruction(new_instruction, old_instruction);
    return new_instruction;

}

static IrInstruction *ir_build_elem_ptr(IrBuilder *irb, AstNode *source_node, IrInstruction *array_ptr,
        IrInstruction *elem_index)
{
    IrInstructionElemPtr *instruction = ir_build_instruction<IrInstructionElemPtr>(irb, source_node);
    instruction->array_ptr = array_ptr;
    instruction->elem_index = elem_index;

    ir_ref_instruction(array_ptr);
    ir_ref_instruction(elem_index);

    return &instruction->base;
}

static IrInstruction *ir_build_elem_ptr_from(IrBuilder *irb, IrInstruction *old_instruction,
        IrInstruction *array_ptr, IrInstruction *elem_index)
{
    IrInstruction *new_instruction = ir_build_elem_ptr(irb, old_instruction->source_node, array_ptr, elem_index);
    ir_link_new_instruction(new_instruction, old_instruction);
    return new_instruction;
}

static IrInstruction *ir_build_field_ptr(IrBuilder *irb, AstNode *source_node,
    IrInstruction *container_ptr, Buf *field_name)
{
    IrInstructionFieldPtr *instruction = ir_build_instruction<IrInstructionFieldPtr>(irb, source_node);
    instruction->container_ptr = container_ptr;
    instruction->field_name = field_name;

    ir_ref_instruction(container_ptr);

    return &instruction->base;
}

//static IrInstruction *ir_build_field_ptr_from(IrBuilder *irb, IrInstruction *old_instruction,
//        IrInstruction *container_ptr, Buf *field_name)
//{
//    IrInstruction *new_instruction = ir_build_field_ptr(irb, old_instruction->source_node, container_ptr, field_name);
//    ir_link_new_instruction(new_instruction, old_instruction);
//    return new_instruction;
//}

static IrInstruction *ir_build_read_field(IrBuilder *irb, AstNode *source_node,
    IrInstruction *container_ptr, Buf *field_name)
{
    IrInstructionReadField *instruction = ir_build_instruction<IrInstructionReadField>(irb, source_node);
    instruction->container_ptr = container_ptr;
    instruction->field_name = field_name;

    ir_ref_instruction(container_ptr);

    return &instruction->base;
}

//static IrInstruction *ir_build_read_field_from(IrBuilder *irb, IrInstruction *old_instruction,
//        IrInstruction *container_ptr, Buf *field_name)
//{
//    IrInstruction *new_instruction = ir_build_read_field(irb, old_instruction->source_node, container_ptr, field_name);
//    ir_link_new_instruction(new_instruction, old_instruction);
//    return new_instruction;
//}

static IrInstruction *ir_build_struct_field_ptr(IrBuilder *irb, AstNode *source_node,
    IrInstruction *struct_ptr, TypeStructField *field)
{
    IrInstructionStructFieldPtr *instruction = ir_build_instruction<IrInstructionStructFieldPtr>(irb, source_node);
    instruction->struct_ptr = struct_ptr;
    instruction->field = field;

    ir_ref_instruction(struct_ptr);

    return &instruction->base;
}

static IrInstruction *ir_build_struct_field_ptr_from(IrBuilder *irb, IrInstruction *old_instruction,
    IrInstruction *struct_ptr, TypeStructField *type_struct_field)
{
    IrInstruction *new_instruction = ir_build_struct_field_ptr(irb, old_instruction->source_node,
        struct_ptr, type_struct_field);
    ir_link_new_instruction(new_instruction, old_instruction);
    return new_instruction;
}

static IrInstruction *ir_build_call(IrBuilder *irb, AstNode *source_node,
        IrInstruction *fn, size_t arg_count, IrInstruction **args)
{
    IrInstructionCall *call_instruction = ir_build_instruction<IrInstructionCall>(irb, source_node);
    call_instruction->fn = fn;
    call_instruction->arg_count = arg_count;
    call_instruction->args = args;

    ir_ref_instruction(fn);
    for (size_t i = 0; i < arg_count; i += 1) {
        ir_ref_instruction(args[i]);
    }

    return &call_instruction->base;
}

static IrInstruction *ir_build_call_from(IrBuilder *irb, IrInstruction *old_instruction,
        IrInstruction *fn, size_t arg_count, IrInstruction **args)
{
    IrInstruction *new_instruction = ir_build_call(irb, old_instruction->source_node, fn, arg_count, args);
    ir_link_new_instruction(new_instruction, old_instruction);
    return new_instruction;
}

static IrInstruction *ir_build_phi(IrBuilder *irb, AstNode *source_node,
        size_t incoming_count, IrBasicBlock **incoming_blocks, IrInstruction **incoming_values)
{
    IrInstructionPhi *phi_instruction = ir_build_instruction<IrInstructionPhi>(irb, source_node);
    phi_instruction->incoming_count = incoming_count;
    phi_instruction->incoming_blocks = incoming_blocks;
    phi_instruction->incoming_values = incoming_values;

    for (size_t i = 0; i < incoming_count; i += 1) {
        ir_ref_instruction(incoming_values[i]);
    }

    return &phi_instruction->base;
}

static IrInstruction *ir_build_phi_from(IrBuilder *irb, IrInstruction *old_instruction,
        size_t incoming_count, IrBasicBlock **incoming_blocks, IrInstruction **incoming_values)
{
    IrInstruction *new_instruction = ir_build_phi(irb, old_instruction->source_node,
            incoming_count, incoming_blocks, incoming_values);
    ir_link_new_instruction(new_instruction, old_instruction);
    return new_instruction;
}

static IrInstruction *ir_build_br(IrBuilder *irb, AstNode *source_node, IrBasicBlock *dest_block, bool is_inline) {
    IrInstructionBr *br_instruction = ir_build_instruction<IrInstructionBr>(irb, source_node);
    br_instruction->base.type_entry = irb->codegen->builtin_types.entry_unreachable;
    br_instruction->base.static_value.ok = true;
    br_instruction->dest_block = dest_block;
    br_instruction->is_inline = is_inline;

    ir_ref_bb(dest_block);

    return &br_instruction->base;
}

static IrInstruction *ir_build_br_from(IrBuilder *irb, IrInstruction *old_instruction, IrBasicBlock *dest_block) {
    IrInstruction *new_instruction = ir_build_br(irb, old_instruction->source_node, dest_block, false);
    ir_link_new_instruction(new_instruction, old_instruction);
    return new_instruction;
}

static IrInstruction *ir_build_un_op(IrBuilder *irb, AstNode *source_node, IrUnOp op_id, IrInstruction *value) {
    IrInstructionUnOp *br_instruction = ir_build_instruction<IrInstructionUnOp>(irb, source_node);
    br_instruction->op_id = op_id;
    br_instruction->value = value;

    ir_ref_instruction(value);

    return &br_instruction->base;
}

static IrInstruction *ir_build_un_op_from(IrBuilder *irb, IrInstruction *old_instruction,
        IrUnOp op_id, IrInstruction *value)
{
    IrInstruction *new_instruction = ir_build_un_op(irb, old_instruction->source_node, op_id, value);
    ir_link_new_instruction(new_instruction, old_instruction);
    return new_instruction;
}

static IrInstruction *ir_build_container_init_list(IrBuilder *irb, AstNode *source_node,
        IrInstruction *container_type, size_t item_count, IrInstruction **items)
{
    IrInstructionContainerInitList *container_init_list_instruction =
        ir_build_instruction<IrInstructionContainerInitList>(irb, source_node);
    container_init_list_instruction->container_type = container_type;
    container_init_list_instruction->item_count = item_count;
    container_init_list_instruction->items = items;

    ir_ref_instruction(container_type);
    for (size_t i = 0; i < item_count; i += 1) {
        ir_ref_instruction(items[i]);
    }

    return &container_init_list_instruction->base;
}

static IrInstruction *ir_build_container_init_fields(IrBuilder *irb, AstNode *source_node,
        IrInstruction *container_type, size_t field_count, Buf **field_names, IrInstruction **field_values)
{
    IrInstructionContainerInitFields *container_init_fields_instruction =
        ir_build_instruction<IrInstructionContainerInitFields>(irb, source_node);
    container_init_fields_instruction->container_type = container_type;
    container_init_fields_instruction->field_count = field_count;
    container_init_fields_instruction->field_names = field_names;
    container_init_fields_instruction->field_values = field_values;

    ir_ref_instruction(container_type);
    for (size_t i = 0; i < field_count; i += 1) {
        ir_ref_instruction(field_values[i]);
    }

    return &container_init_fields_instruction->base;
}

static IrInstruction *ir_build_unreachable(IrBuilder *irb, AstNode *source_node) {
    IrInstructionUnreachable *unreachable_instruction =
        ir_build_instruction<IrInstructionUnreachable>(irb, source_node);
    unreachable_instruction->base.static_value.ok = true;
    unreachable_instruction->base.type_entry = irb->codegen->builtin_types.entry_unreachable;
    return &unreachable_instruction->base;
}

static IrInstruction *ir_build_unreachable_from(IrBuilder *irb, IrInstruction *old_instruction) {
    IrInstruction *new_instruction = ir_build_unreachable(irb, old_instruction->source_node);
    ir_link_new_instruction(new_instruction, old_instruction);
    return new_instruction;
}

static IrInstruction *ir_build_store_ptr(IrBuilder *irb, AstNode *source_node,
        IrInstruction *ptr, IrInstruction *value)
{
    IrInstructionStorePtr *instruction = ir_build_instruction<IrInstructionStorePtr>(irb, source_node);
    instruction->base.static_value.ok = true;
    instruction->base.type_entry = irb->codegen->builtin_types.entry_void;
    instruction->ptr = ptr;
    instruction->value = value;

    ir_ref_instruction(ptr);
    ir_ref_instruction(value);

    return &instruction->base;
}

static IrInstruction *ir_build_store_ptr_from(IrBuilder *irb, IrInstruction *old_instruction,
        IrInstruction *ptr, IrInstruction *value)
{
    IrInstruction *new_instruction = ir_build_store_ptr(irb, old_instruction->source_node, ptr, value);
    ir_link_new_instruction(new_instruction, old_instruction);
    return new_instruction;
}

static IrInstruction *ir_build_var_decl(IrBuilder *irb, AstNode *source_node,
        VariableTableEntry *var, IrInstruction *var_type, IrInstruction *init_value)
{
    IrInstructionDeclVar *decl_var_instruction = ir_build_instruction<IrInstructionDeclVar>(irb, source_node);
    decl_var_instruction->base.static_value.ok = true;
    decl_var_instruction->base.type_entry = irb->codegen->builtin_types.entry_void;
    decl_var_instruction->var = var;
    decl_var_instruction->var_type = var_type;
    decl_var_instruction->init_value = init_value;

    if (var_type) ir_ref_instruction(var_type);
    ir_ref_instruction(init_value);

    return &decl_var_instruction->base;
}

static IrInstruction *ir_build_var_decl_from(IrBuilder *irb, IrInstruction *old_instruction,
        VariableTableEntry *var, IrInstruction *var_type, IrInstruction *init_value)
{
    IrInstruction *new_instruction = ir_build_var_decl(irb, old_instruction->source_node, var, var_type, init_value);
    ir_link_new_instruction(new_instruction, old_instruction);
    return new_instruction;
}

static IrInstruction *ir_build_load_ptr(IrBuilder *irb, AstNode *source_node, IrInstruction *ptr) {
    IrInstructionLoadPtr *instruction = ir_build_instruction<IrInstructionLoadPtr>(irb, source_node);
    instruction->ptr = ptr;

    ir_ref_instruction(ptr);

    return &instruction->base;
}

static IrInstruction *ir_build_load_ptr_from(IrBuilder *irb, IrInstruction *old_instruction, IrInstruction *ptr) {
    IrInstruction *new_instruction = ir_build_load_ptr(irb, old_instruction->source_node, ptr);
    ir_link_new_instruction(new_instruction, old_instruction);
    return new_instruction;
}

static IrInstruction *ir_build_typeof(IrBuilder *irb, AstNode *source_node, IrInstruction *value) {
    IrInstructionTypeOf *instruction = ir_build_instruction<IrInstructionTypeOf>(irb, source_node);
    instruction->value = value;

    ir_ref_instruction(value);

    return &instruction->base;
}

static IrInstruction *ir_build_to_ptr_type(IrBuilder *irb, AstNode *source_node, IrInstruction *value) {
    IrInstructionToPtrType *instruction = ir_build_instruction<IrInstructionToPtrType>(irb, source_node);
    instruction->value = value;

    ir_ref_instruction(value);

    return &instruction->base;
}

static IrInstruction *ir_build_ptr_type_child(IrBuilder *irb, AstNode *source_node, IrInstruction *value) {
    IrInstructionPtrTypeChild *instruction = ir_build_instruction<IrInstructionPtrTypeChild>(irb, source_node);
    instruction->value = value;

    ir_ref_instruction(value);

    return &instruction->base;
}

static IrInstruction *ir_build_set_fn_test(IrBuilder *irb, AstNode *source_node, IrInstruction *fn_value,
        IrInstruction *is_test)
{
    IrInstructionSetFnTest *instruction = ir_build_instruction<IrInstructionSetFnTest>(irb, source_node);
    instruction->fn_value = fn_value;
    instruction->is_test = is_test;

    ir_ref_instruction(fn_value);
    ir_ref_instruction(is_test);

    return &instruction->base;
}

static IrInstruction *ir_build_array_type(IrBuilder *irb, AstNode *source_node, IrInstruction *size,
        IrInstruction *child_type)
{
    IrInstructionArrayType *instruction = ir_build_instruction<IrInstructionArrayType>(irb, source_node);
    instruction->size = size;
    instruction->child_type = child_type;

    ir_ref_instruction(size);
    ir_ref_instruction(child_type);

    return &instruction->base;
}

static IrInstruction *ir_build_slice_type(IrBuilder *irb, AstNode *source_node, bool is_const,
        IrInstruction *child_type)
{
    IrInstructionSliceType *instruction = ir_build_instruction<IrInstructionSliceType>(irb, source_node);
    instruction->is_const = is_const;
    instruction->child_type = child_type;

    ir_ref_instruction(child_type);

    return &instruction->base;
}

static void ir_gen_defers_for_block(IrBuilder *irb, BlockContext *inner_block, BlockContext *outer_block,
        bool gen_error_defers, bool gen_maybe_defers)
{
    while (inner_block != outer_block) {
        if (inner_block->node->type == NodeTypeDefer &&
           ((inner_block->node->data.defer.kind == ReturnKindUnconditional) ||
            (gen_error_defers && inner_block->node->data.defer.kind == ReturnKindError) ||
            (gen_maybe_defers && inner_block->node->data.defer.kind == ReturnKindMaybe)))
        {
            AstNode *defer_expr_node = inner_block->node->data.defer.expr;
            ir_gen_node(irb, defer_expr_node, defer_expr_node->block_context);
        }
        inner_block = inner_block->parent;
    }
}

static IrInstruction *ir_gen_return(IrBuilder *irb, AstNode *node) {
    assert(node->type == NodeTypeReturnExpr);

    BlockContext *scope = node->block_context;

    if (!scope->fn_entry) {
        add_node_error(irb->codegen, node, buf_sprintf("return expression outside function definition"));
        return irb->codegen->invalid_instruction;
    }

    AstNode *expr_node = node->data.return_expr.expr;
    switch (node->data.return_expr.kind) {
        case ReturnKindUnconditional:
            {
                IrInstruction *return_value;
                if (expr_node) {
                    return_value = ir_gen_node(irb, expr_node, scope);
                } else {
                    return_value = ir_build_const_void(irb, node);
                }

                return ir_build_return(irb, node, return_value);
            }
        case ReturnKindError:
            zig_panic("TODO %%return");
        case ReturnKindMaybe:
            zig_panic("TODO ?return");
    }
    zig_unreachable();
}

static void ir_set_cursor_at_end(IrBuilder *irb, IrBasicBlock *basic_block) {
    assert(basic_block);

    irb->current_basic_block = basic_block;
}

static VariableTableEntry *add_local_var(CodeGen *codegen, AstNode *node, BlockContext *scope,
        Buf *name, bool src_is_const, bool gen_is_const, bool is_shadowable, bool is_inline)
{
    VariableTableEntry *variable_entry = allocate<VariableTableEntry>(1);
    variable_entry->block_context = scope;
    variable_entry->import = node->owner;
    variable_entry->shadowable = is_shadowable;
    variable_entry->mem_slot_index = SIZE_MAX;
    variable_entry->is_inline = is_inline;

    if (name) {
        buf_init_from_buf(&variable_entry->name, name);

        VariableTableEntry *existing_var = find_variable(codegen, node->block_context, name);
        if (existing_var && !existing_var->shadowable) {
            ErrorMsg *msg = add_node_error(codegen, node,
                    buf_sprintf("redeclaration of variable '%s'", buf_ptr(name)));
            add_error_note(codegen, msg, existing_var->decl_node, buf_sprintf("previous declaration is here"));
            variable_entry->type = codegen->builtin_types.entry_invalid;
        } else {
            auto primitive_table_entry = codegen->primitive_type_table.maybe_get(name);
            if (primitive_table_entry) {
                TypeTableEntry *type = primitive_table_entry->value;
                add_node_error(codegen, node,
                        buf_sprintf("variable shadows type '%s'", buf_ptr(&type->name)));
                variable_entry->type = codegen->builtin_types.entry_invalid;
            } else {
                AstNode *decl_node = find_decl(node->block_context, name);
                if (decl_node && decl_node->type != NodeTypeVariableDeclaration) {
                    ErrorMsg *msg = add_node_error(codegen, node,
                            buf_sprintf("redefinition of '%s'", buf_ptr(name)));
                    add_error_note(codegen, msg, decl_node, buf_sprintf("previous definition is here"));
                    variable_entry->type = codegen->builtin_types.entry_invalid;
                }
            }
        }

        node->block_context->var_table.put(&variable_entry->name, variable_entry);
    } else {
        assert(is_shadowable);
        // TODO replace _anon with @anon and make sure all tests still pass
        buf_init_from_str(&variable_entry->name, "_anon");
    }

    variable_entry->src_is_const = src_is_const;
    variable_entry->gen_is_const = gen_is_const;
    variable_entry->decl_node = node;

    return variable_entry;
}

// Set name to nullptr to make the variable anonymous (not visible to programmer).
static VariableTableEntry *ir_add_local_var(IrBuilder *irb, AstNode *node, BlockContext *scope, Buf *name,
        bool src_is_const, bool gen_is_const, bool is_shadowable, bool is_inline)
{
    VariableTableEntry *var = add_local_var(irb->codegen, node, scope, name,
            src_is_const, gen_is_const, is_shadowable, is_inline);
    if (is_inline || gen_is_const)
        var->mem_slot_index = exec_next_mem_slot(irb->exec);
    return var;
}

static IrInstruction *ir_gen_block(IrBuilder *irb, AstNode *block_node) {
    assert(block_node->type == NodeTypeBlock);

    BlockContext *parent_context = block_node->block_context;
    BlockContext *outer_block_context = new_block_context(block_node, parent_context);
    BlockContext *child_context = outer_block_context;

    IrInstruction *return_value = nullptr;
    for (size_t i = 0; i < block_node->data.block.statements.length; i += 1) {
        AstNode *statement_node = block_node->data.block.statements.at(i);
        return_value = ir_gen_node(irb, statement_node, child_context);
        if (statement_node->type == NodeTypeDefer && return_value != irb->codegen->invalid_instruction) {
            // defer starts a new block context
            child_context = statement_node->data.defer.child_block;
            assert(child_context);
        }
    }

    if (!return_value)
        return_value = ir_build_const_void(irb, block_node);

    ir_gen_defers_for_block(irb, child_context, outer_block_context, false, false);

    return return_value;
}

static IrInstruction *ir_gen_bin_op_id(IrBuilder *irb, AstNode *node, IrBinOp op_id) {
    IrInstruction *op1 = ir_gen_node(irb, node->data.bin_op_expr.op1, node->block_context);
    IrInstruction *op2 = ir_gen_node(irb, node->data.bin_op_expr.op2, node->block_context);
    return ir_build_bin_op(irb, node, op_id, op1, op2);
}

static IrInstruction *ir_gen_assign(IrBuilder *irb, AstNode *node) {
    IrInstruction *lvalue = ir_gen_node_extra(irb, node->data.bin_op_expr.op1, node->block_context, LValPurposeAssign);
    if (lvalue == irb->codegen->invalid_instruction)
        return lvalue;

    IrInstruction *rvalue = ir_gen_node(irb, node->data.bin_op_expr.op2, node->block_context);
    if (rvalue == irb->codegen->invalid_instruction)
        return rvalue;

    ir_build_store_ptr(irb, node, lvalue, rvalue);
    return ir_build_const_void(irb, node);
}

static IrInstruction *ir_gen_assign_op(IrBuilder *irb, AstNode *node, IrBinOp op_id) {
    IrInstruction *lvalue = ir_gen_node_extra(irb, node->data.bin_op_expr.op1, node->block_context, LValPurposeAssign);
    if (lvalue == irb->codegen->invalid_instruction)
        return lvalue;
    IrInstruction *op1 = ir_build_load_ptr(irb, node->data.bin_op_expr.op1, lvalue);
    IrInstruction *op2 = ir_gen_node(irb, node->data.bin_op_expr.op2, node->block_context);
    if (op2 == irb->codegen->invalid_instruction)
        return op2;
    IrInstruction *result = ir_build_bin_op(irb, node, op_id, op1, op2);
    ir_build_store_ptr(irb, node, lvalue, result);
    return ir_build_const_void(irb, node);
}

static IrInstruction *ir_gen_bin_op(IrBuilder *irb, AstNode *node) {
    assert(node->type == NodeTypeBinOpExpr);

    BinOpType bin_op_type = node->data.bin_op_expr.bin_op;
    switch (bin_op_type) {
        case BinOpTypeInvalid:
            zig_unreachable();
        case BinOpTypeAssign:
            return ir_gen_assign(irb, node);
        case BinOpTypeAssignTimes:
            return ir_gen_assign_op(irb, node, IrBinOpMult);
        case BinOpTypeAssignTimesWrap:
            return ir_gen_assign_op(irb, node, IrBinOpMultWrap);
        case BinOpTypeAssignDiv:
            return ir_gen_assign_op(irb, node, IrBinOpDiv);
        case BinOpTypeAssignMod:
            return ir_gen_assign_op(irb, node, IrBinOpMod);
        case BinOpTypeAssignPlus:
            return ir_gen_assign_op(irb, node, IrBinOpAdd);
        case BinOpTypeAssignPlusWrap:
            return ir_gen_assign_op(irb, node, IrBinOpAddWrap);
        case BinOpTypeAssignMinus:
            return ir_gen_assign_op(irb, node, IrBinOpSub);
        case BinOpTypeAssignMinusWrap:
            return ir_gen_assign_op(irb, node, IrBinOpSubWrap);
        case BinOpTypeAssignBitShiftLeft:
            return ir_gen_assign_op(irb, node, IrBinOpBitShiftLeft);
        case BinOpTypeAssignBitShiftLeftWrap:
            return ir_gen_assign_op(irb, node, IrBinOpBitShiftLeftWrap);
        case BinOpTypeAssignBitShiftRight:
            return ir_gen_assign_op(irb, node, IrBinOpBitShiftRight);
        case BinOpTypeAssignBitAnd:
            return ir_gen_assign_op(irb, node, IrBinOpBinAnd);
        case BinOpTypeAssignBitXor:
            return ir_gen_assign_op(irb, node, IrBinOpBinXor);
        case BinOpTypeAssignBitOr:
            return ir_gen_assign_op(irb, node, IrBinOpBinOr);
        case BinOpTypeAssignBoolAnd:
            return ir_gen_assign_op(irb, node, IrBinOpBoolAnd);
        case BinOpTypeAssignBoolOr:
            return ir_gen_assign_op(irb, node, IrBinOpBoolOr);
        case BinOpTypeBoolOr:
        case BinOpTypeBoolAnd:
            // note: this is not a direct mapping to IrBinOpBoolOr/And
            // because of the control flow
            zig_panic("TODO gen IR for bool or/and");
        case BinOpTypeCmpEq:
            return ir_gen_bin_op_id(irb, node, IrBinOpCmpEq);
        case BinOpTypeCmpNotEq:
            return ir_gen_bin_op_id(irb, node, IrBinOpCmpNotEq);
        case BinOpTypeCmpLessThan:
            return ir_gen_bin_op_id(irb, node, IrBinOpCmpLessThan);
        case BinOpTypeCmpGreaterThan:
            return ir_gen_bin_op_id(irb, node, IrBinOpCmpGreaterThan);
        case BinOpTypeCmpLessOrEq:
            return ir_gen_bin_op_id(irb, node, IrBinOpCmpLessOrEq);
        case BinOpTypeCmpGreaterOrEq:
            return ir_gen_bin_op_id(irb, node, IrBinOpCmpGreaterOrEq);
        case BinOpTypeBinOr:
            return ir_gen_bin_op_id(irb, node, IrBinOpBinOr);
        case BinOpTypeBinXor:
            return ir_gen_bin_op_id(irb, node, IrBinOpBinXor);
        case BinOpTypeBinAnd:
            return ir_gen_bin_op_id(irb, node, IrBinOpBinAnd);
        case BinOpTypeBitShiftLeft:
            return ir_gen_bin_op_id(irb, node, IrBinOpBitShiftLeft);
        case BinOpTypeBitShiftLeftWrap:
            return ir_gen_bin_op_id(irb, node, IrBinOpBitShiftLeftWrap);
        case BinOpTypeBitShiftRight:
            return ir_gen_bin_op_id(irb, node, IrBinOpBitShiftRight);
        case BinOpTypeAdd:
            return ir_gen_bin_op_id(irb, node, IrBinOpAdd);
        case BinOpTypeAddWrap:
            return ir_gen_bin_op_id(irb, node, IrBinOpAddWrap);
        case BinOpTypeSub:
            return ir_gen_bin_op_id(irb, node, IrBinOpSub);
        case BinOpTypeSubWrap:
            return ir_gen_bin_op_id(irb, node, IrBinOpSubWrap);
        case BinOpTypeMult:
            return ir_gen_bin_op_id(irb, node, IrBinOpMult);
        case BinOpTypeMultWrap:
            return ir_gen_bin_op_id(irb, node, IrBinOpMultWrap);
        case BinOpTypeDiv:
            return ir_gen_bin_op_id(irb, node, IrBinOpDiv);
        case BinOpTypeMod:
            return ir_gen_bin_op_id(irb, node, IrBinOpMod);
        case BinOpTypeArrayCat:
            return ir_gen_bin_op_id(irb, node, IrBinOpArrayCat);
        case BinOpTypeArrayMult:
            return ir_gen_bin_op_id(irb, node, IrBinOpArrayMult);
        case BinOpTypeUnwrapMaybe:
            zig_panic("TODO gen IR for unwrap maybe");
    }
    zig_unreachable();
}

static IrInstruction *ir_gen_num_lit(IrBuilder *irb, AstNode *node) {
    assert(node->type == NodeTypeNumberLiteral);

    if (node->data.number_literal.overflow) {
        add_node_error(irb->codegen, node, buf_sprintf("number literal too large to be represented in any type"));
        return irb->codegen->invalid_instruction;
    }

    return ir_build_const_bignum(irb, node, node->data.number_literal.bignum);
}

static IrInstruction *ir_gen_decl_ref(IrBuilder *irb, AstNode *source_node, AstNode *decl_node,
        LValPurpose lval, BlockContext *scope)
{
    resolve_top_level_decl(irb->codegen, decl_node, lval);
    TopLevelDecl *tld = get_as_top_level_decl(decl_node);
    if (tld->resolution == TldResolutionInvalid)
        return irb->codegen->invalid_instruction;

    if (decl_node->type == NodeTypeVariableDeclaration) {
        VariableTableEntry *var = decl_node->data.variable_declaration.variable;
        IrInstruction *var_ptr = ir_build_var_ptr(irb, source_node, var);
        return ir_build_load_ptr(irb, source_node, var_ptr);
    } else if (decl_node->type == NodeTypeFnProto) {
        FnTableEntry *fn_entry = decl_node->data.fn_proto.fn_table_entry;
        assert(fn_entry->type_entry);
        if (fn_entry->type_entry->id == TypeTableEntryIdGenericFn) {
            return ir_build_const_generic_fn(irb, source_node, fn_entry->type_entry);
        } else {
            return ir_build_const_fn(irb, source_node, fn_entry);
        }
    } else if (decl_node->type == NodeTypeContainerDecl) {
        if (decl_node->data.struct_decl.generic_params.length > 0) {
            TypeTableEntry *type_entry = decl_node->data.struct_decl.generic_fn_type;
            assert(type_entry);
            return ir_build_const_generic_fn(irb, source_node, type_entry);
        } else {
            return ir_build_const_type(irb, source_node, decl_node->data.struct_decl.type_entry);
        }
    } else if (decl_node->type == NodeTypeTypeDecl) {
        return ir_build_const_type(irb, source_node, decl_node->data.type_decl.child_type_entry);
    } else {
        zig_unreachable();
    }
}

static IrInstruction *ir_gen_symbol(IrBuilder *irb, AstNode *node, LValPurpose lval) {
    assert(node->type == NodeTypeSymbol);

    Buf *variable_name = node->data.symbol_expr.symbol;

    auto primitive_table_entry = irb->codegen->primitive_type_table.maybe_get(variable_name);
    if (primitive_table_entry) {
        return ir_build_const_type(irb, node, primitive_table_entry->value);
    }

    VariableTableEntry *var = find_variable(irb->codegen, node->block_context, variable_name);
    if (var) {
        IrInstruction *var_ptr = ir_build_var_ptr(irb, node, var);
        if (lval != LValPurposeNone)
            return var_ptr;
        else
            return ir_build_load_ptr(irb, node, var_ptr);
    }

    AstNode *decl_node = find_decl(node->block_context, variable_name);
    if (decl_node) {
        return ir_gen_decl_ref(irb, node, decl_node, lval, node->block_context);
    }

    if (node->owner->any_imports_failed) {
        // skip the error message since we had a failing import in this file
        // if an import breaks we don't need redundant undeclared identifier errors
        return irb->codegen->invalid_instruction;
    }

    add_node_error(irb->codegen, node, buf_sprintf("use of undeclared identifier '%s'", buf_ptr(variable_name)));
    return irb->codegen->invalid_instruction;
}

static IrInstruction *ir_gen_array_access(IrBuilder *irb, AstNode *node, LValPurpose lval) {
    assert(node->type == NodeTypeArrayAccessExpr);

    AstNode *array_ref_node = node->data.array_access_expr.array_ref_expr;
    IrInstruction *array_ref_instruction = ir_gen_node(irb, array_ref_node, node->block_context);
    if (array_ref_instruction == irb->codegen->invalid_instruction)
        return array_ref_instruction;

    AstNode *subscript_node = node->data.array_access_expr.subscript;
    IrInstruction *subscript_instruction = ir_gen_node(irb, subscript_node, node->block_context);
    if (subscript_instruction == irb->codegen->invalid_instruction)
        return subscript_instruction;

    IrInstruction *ptr_instruction = ir_build_elem_ptr(irb, node, array_ref_instruction, subscript_instruction);
    if (lval != LValPurposeNone)
        return ptr_instruction;

    return ir_build_load_ptr(irb, node, ptr_instruction);
}

static IrInstruction *ir_gen_field_access(IrBuilder *irb, AstNode *node, LValPurpose lval) {
    assert(node->type == NodeTypeFieldAccessExpr);

    AstNode *container_ref_node = node->data.field_access_expr.struct_expr;
    Buf *field_name = node->data.field_access_expr.field_name;

    IrInstruction *container_ref_instruction = ir_gen_node(irb, container_ref_node, node->block_context);
    if (container_ref_instruction == irb->codegen->invalid_instruction)
        return container_ref_instruction;

    if (lval == LValPurposeNone) {
        return ir_build_read_field(irb, node, container_ref_instruction, field_name);
    } else {
        return ir_build_field_ptr(irb, node, container_ref_instruction, field_name);
    }
}

static IrInstruction *ir_gen_builtin_fn_call(IrBuilder *irb, AstNode *node) {
    assert(node->type == NodeTypeFnCallExpr);

    AstNode *fn_ref_expr = node->data.fn_call_expr.fn_ref_expr;
    Buf *name = fn_ref_expr->data.symbol_expr.symbol;
    auto entry = irb->codegen->builtin_fn_table.maybe_get(name);

    if (!entry) {
        add_node_error(irb->codegen, node,
                buf_sprintf("invalid builtin function: '%s'", buf_ptr(name)));
        return irb->codegen->invalid_instruction;
    }

    BuiltinFnEntry *builtin_fn = entry->value;
    size_t actual_param_count = node->data.fn_call_expr.params.length;

    if (builtin_fn->param_count != actual_param_count) {
        add_node_error(irb->codegen, node,
                buf_sprintf("expected %zu arguments, got %zu",
                    builtin_fn->param_count, actual_param_count));
        return irb->codegen->invalid_instruction;
    }

    builtin_fn->ref_count += 1;

    switch (builtin_fn->id) {
        case BuiltinFnIdInvalid:
            zig_unreachable();
        case BuiltinFnIdUnreachable:
            return ir_build_unreachable(irb, node);
        case BuiltinFnIdTypeof:
            {
                AstNode *arg_node = node->data.fn_call_expr.params.at(0);
                IrInstruction *arg = ir_gen_node(irb, arg_node, node->block_context);
                if (arg == irb->codegen->invalid_instruction)
                    return arg;
                return ir_build_typeof(irb, node, arg);
            }
        case BuiltinFnIdSetFnTest:
            {
                AstNode *arg0_node = node->data.fn_call_expr.params.at(0);
                IrInstruction *arg0_value = ir_gen_node(irb, arg0_node, node->block_context);
                if (arg0_value == irb->codegen->invalid_instruction)
                    return arg0_value;

                AstNode *arg1_node = node->data.fn_call_expr.params.at(1);
                IrInstruction *arg1_value = ir_gen_node(irb, arg1_node, node->block_context);
                if (arg1_value == irb->codegen->invalid_instruction)
                    return arg1_value;

                return ir_build_set_fn_test(irb, node, arg0_value, arg1_value);
            }
        case BuiltinFnIdMemcpy:
        case BuiltinFnIdMemset:
        case BuiltinFnIdSizeof:
        case BuiltinFnIdAlignof:
        case BuiltinFnIdMaxValue:
        case BuiltinFnIdMinValue:
        case BuiltinFnIdMemberCount:
        case BuiltinFnIdAddWithOverflow:
        case BuiltinFnIdSubWithOverflow:
        case BuiltinFnIdMulWithOverflow:
        case BuiltinFnIdShlWithOverflow:
        case BuiltinFnIdCInclude:
        case BuiltinFnIdCDefine:
        case BuiltinFnIdCUndef:
        case BuiltinFnIdCompileVar:
        case BuiltinFnIdCompileErr:
        case BuiltinFnIdConstEval:
        case BuiltinFnIdCtz:
        case BuiltinFnIdClz:
        case BuiltinFnIdImport:
        case BuiltinFnIdCImport:
        case BuiltinFnIdErrName:
        case BuiltinFnIdBreakpoint:
        case BuiltinFnIdReturnAddress:
        case BuiltinFnIdFrameAddress:
        case BuiltinFnIdEmbedFile:
        case BuiltinFnIdCmpExchange:
        case BuiltinFnIdFence:
        case BuiltinFnIdDivExact:
        case BuiltinFnIdTruncate:
        case BuiltinFnIdIntType:
        case BuiltinFnIdSetFnVisible:
        case BuiltinFnIdSetFnStaticEval:
        case BuiltinFnIdSetFnNoInline:
        case BuiltinFnIdSetDebugSafety:
            zig_panic("TODO IR gen more builtin functions");
    }
    zig_unreachable();
}

static IrInstruction *ir_gen_fn_call(IrBuilder *irb, AstNode *node) {
    assert(node->type == NodeTypeFnCallExpr);

    if (node->data.fn_call_expr.is_builtin)
        return ir_gen_builtin_fn_call(irb, node);

    AstNode *fn_ref_node = node->data.fn_call_expr.fn_ref_expr;
    IrInstruction *fn = ir_gen_node(irb, fn_ref_node, node->block_context);
    if (fn == irb->codegen->invalid_instruction)
        return fn;

    size_t arg_count = node->data.fn_call_expr.params.length;
    IrInstruction **args = allocate<IrInstruction*>(arg_count);
    for (size_t i = 0; i < arg_count; i += 1) {
        AstNode *arg_node = node->data.fn_call_expr.params.at(i);
        args[i] = ir_gen_node(irb, arg_node, node->block_context);
    }

    return ir_build_call(irb, node, fn, arg_count, args);
}

static IrInstruction *ir_gen_if_bool_expr(IrBuilder *irb, AstNode *node) {
    assert(node->type == NodeTypeIfBoolExpr);

    IrInstruction *condition = ir_gen_node(irb, node->data.if_bool_expr.condition, node->block_context);
    if (condition == irb->codegen->invalid_instruction)
        return condition;

    AstNode *then_node = node->data.if_bool_expr.then_block;
    AstNode *else_node = node->data.if_bool_expr.else_node;

    IrBasicBlock *then_block = ir_build_basic_block(irb, "Then");
    IrBasicBlock *else_block = ir_build_basic_block(irb, "Else");
    IrBasicBlock *endif_block = ir_build_basic_block(irb, "EndIf");

    ir_build_cond_br(irb, condition->source_node, condition, then_block, else_block, false);

    ir_set_cursor_at_end(irb, then_block);
    IrInstruction *then_expr_result = ir_gen_node(irb, then_node, node->block_context);
    if (then_expr_result == irb->codegen->invalid_instruction)
        return then_expr_result;
    IrBasicBlock *after_then_block = irb->current_basic_block;
    ir_build_br(irb, node, endif_block, false);

    ir_set_cursor_at_end(irb, else_block);
    IrInstruction *else_expr_result;
    if (else_node) {
        else_expr_result = ir_gen_node(irb, else_node, node->block_context);
        if (else_expr_result == irb->codegen->invalid_instruction)
            return else_expr_result;
    } else {
        else_expr_result = ir_build_const_void(irb, node);
    }
    IrBasicBlock *after_else_block = irb->current_basic_block;
    ir_build_br(irb, node, endif_block, false);

    ir_set_cursor_at_end(irb, endif_block);
    IrInstruction **incoming_values = allocate<IrInstruction *>(2);
    incoming_values[0] = then_expr_result;
    incoming_values[1] = else_expr_result;
    IrBasicBlock **incoming_blocks = allocate<IrBasicBlock *>(2);
    incoming_blocks[0] = after_then_block;
    incoming_blocks[1] = after_else_block;

    return ir_build_phi(irb, node, 2, incoming_blocks, incoming_values);
}

static IrInstruction *ir_gen_prefix_op_id(IrBuilder *irb, AstNode *node, IrUnOp op_id) {
    assert(node->type == NodeTypePrefixOpExpr);
    AstNode *expr_node = node->data.prefix_op_expr.primary_expr;

    IrInstruction *value = ir_gen_node(irb, expr_node, node->block_context);
    if (value == irb->codegen->invalid_instruction)
        return value;

    return ir_build_un_op(irb, node, op_id, value);
}

static IrInstruction *ir_gen_prefix_op_expr(IrBuilder *irb, AstNode *node) {
    assert(node->type == NodeTypePrefixOpExpr);

    PrefixOp prefix_op = node->data.prefix_op_expr.prefix_op;
    //AstNode *expr_node = node->data.prefix_op_expr.primary_expr;

    switch (prefix_op) {
        case PrefixOpInvalid:
            zig_unreachable();
        case PrefixOpBoolNot:
            return ir_gen_prefix_op_id(irb, node, IrUnOpBoolNot);
        case PrefixOpBinNot:
            return ir_gen_prefix_op_id(irb, node, IrUnOpBinNot);
        case PrefixOpNegation:
            return ir_gen_prefix_op_id(irb, node, IrUnOpNegation);
        case PrefixOpNegationWrap:
            return ir_gen_prefix_op_id(irb, node, IrUnOpNegationWrap);
        case PrefixOpAddressOf:
            return ir_gen_prefix_op_id(irb, node, IrUnOpAddressOf);
        case PrefixOpConstAddressOf:
            return ir_gen_prefix_op_id(irb, node, IrUnOpConstAddressOf);
        case PrefixOpDereference:
            return ir_gen_prefix_op_id(irb, node, IrUnOpDereference);
        case PrefixOpMaybe:
            return ir_gen_prefix_op_id(irb, node, IrUnOpMaybe);
        case PrefixOpError:
            return ir_gen_prefix_op_id(irb, node, IrUnOpError);
        case PrefixOpUnwrapError:
            return ir_gen_prefix_op_id(irb, node, IrUnOpUnwrapError);
        case PrefixOpUnwrapMaybe:
            return ir_gen_prefix_op_id(irb, node, IrUnOpUnwrapMaybe);
    }
    zig_unreachable();
}

static IrInstruction *ir_gen_container_init_expr(IrBuilder *irb, AstNode *node) {
    assert(node->type == NodeTypeContainerInitExpr);

    AstNodeContainerInitExpr *container_init_expr = &node->data.container_init_expr;
    ContainerInitKind kind = container_init_expr->kind;

    IrInstruction *container_type = ir_gen_node(irb, container_init_expr->type, node->block_context);
    if (container_type == irb->codegen->invalid_instruction)
        return container_type;

    if (kind == ContainerInitKindStruct) {
        size_t field_count = container_init_expr->entries.length;
        IrInstruction **values = allocate<IrInstruction *>(field_count);
        Buf **names = allocate<Buf *>(field_count);
        for (size_t i = 0; i < field_count; i += 1) {
            AstNode *entry_node = container_init_expr->entries.at(i);
            assert(entry_node->type == NodeTypeStructValueField);

            Buf *name = entry_node->data.struct_val_field.name;
            AstNode *expr_node = entry_node->data.struct_val_field.expr;
            IrInstruction *expr_value = ir_gen_node(irb, expr_node, node->block_context);
            if (expr_value == irb->codegen->invalid_instruction)
                return expr_value;

            names[i] = name;
            values[i] = expr_value;
        }
        return ir_build_container_init_fields(irb, node, container_type, field_count, names, values);
    } else if (kind == ContainerInitKindArray) {
        size_t item_count = container_init_expr->entries.length;
        IrInstruction **values = allocate<IrInstruction *>(item_count);
        for (size_t i = 0; i < item_count; i += 1) {
            AstNode *expr_node = container_init_expr->entries.at(i);
            IrInstruction *expr_value = ir_gen_node(irb, expr_node, node->block_context);
            if (expr_value == irb->codegen->invalid_instruction)
                return expr_value;

            values[i] = expr_value;
        }
        return ir_build_container_init_list(irb, node, container_type, item_count, values);
    } else {
        zig_unreachable();
    }
}

static IrInstruction *ir_gen_var_decl(IrBuilder *irb, AstNode *node) {
    assert(node->type == NodeTypeVariableDeclaration);

    AstNodeVariableDeclaration *variable_declaration = &node->data.variable_declaration;

    IrInstruction *type_instruction;
    if (variable_declaration->type != nullptr) {
        type_instruction = ir_gen_node(irb, variable_declaration->type, node->block_context);
        if (type_instruction == irb->codegen->invalid_instruction)
            return type_instruction;
    } else {
        type_instruction = nullptr;
    }

    IrInstruction *init_value = ir_gen_node(irb, variable_declaration->expr, node->block_context);
    if (init_value == irb->codegen->invalid_instruction)
        return init_value;

    bool is_shadowable = false;
    bool is_const = variable_declaration->is_const;
    bool is_extern = variable_declaration->is_extern;
    bool is_inline = variable_declaration->is_inline;
    VariableTableEntry *var = ir_add_local_var(irb, node, node->block_context,
            variable_declaration->symbol, is_const, is_const, is_shadowable, is_inline);

    if (!is_extern && !variable_declaration->expr) {
        var->type = irb->codegen->builtin_types.entry_invalid;
        add_node_error(irb->codegen, node, buf_sprintf("variables must be initialized"));
        return irb->codegen->invalid_instruction;
    }

    return ir_build_var_decl(irb, node, var, type_instruction, init_value);
}

static IrInstruction *ir_gen_while_expr(IrBuilder *irb, AstNode *node) {
    assert(node->type == NodeTypeWhileExpr);

    AstNode *continue_expr_node = node->data.while_expr.continue_expr;

    IrBasicBlock *cond_block = ir_build_basic_block(irb, "WhileCond");
    IrBasicBlock *body_block = ir_build_basic_block(irb, "WhileBody");
    IrBasicBlock *continue_block = continue_expr_node ?
        ir_build_basic_block(irb, "WhileContinue") : cond_block;
    IrBasicBlock *end_block = ir_build_basic_block(irb, "WhileEnd");

    bool is_inline = node->data.while_expr.is_inline;
    ir_build_br(irb, node, cond_block, is_inline);

    if (continue_expr_node) {
        ir_set_cursor_at_end(irb, continue_block);
        ir_gen_node(irb, continue_expr_node, node->block_context);
        ir_build_br(irb, node, cond_block, is_inline);
    }

    ir_set_cursor_at_end(irb, cond_block);
    IrInstruction *cond_val = ir_gen_node(irb, node->data.while_expr.condition, node->block_context);
    ir_build_cond_br(irb, node->data.while_expr.condition, cond_val, body_block, end_block, is_inline);

    ir_set_cursor_at_end(irb, body_block);

    irb->break_block_stack.append(end_block);
    irb->continue_block_stack.append(continue_block);
    ir_gen_node(irb, node->data.while_expr.body, node->block_context);
    irb->break_block_stack.pop();
    irb->continue_block_stack.pop();

    ir_build_br(irb, node, continue_block, is_inline);
    ir_set_cursor_at_end(irb, end_block);

    return ir_build_const_void(irb, node);
}

static IrInstruction *ir_gen_for_expr(IrBuilder *irb, AstNode *node) {
    assert(node->type == NodeTypeForExpr);

    BlockContext *parent_scope = node->block_context;

    AstNode *array_node = node->data.for_expr.array_expr;
    AstNode *elem_node = node->data.for_expr.elem_node;
    AstNode *index_node = node->data.for_expr.index_node;
    AstNode *body_node = node->data.for_expr.body;

    if (!elem_node) {
        add_node_error(irb->codegen, node, buf_sprintf("for loop expression missing element parameter"));
        return irb->codegen->invalid_instruction;
    }
    assert(elem_node->type == NodeTypeSymbol);

    IrInstruction *array_val = ir_gen_node(irb, array_node, parent_scope);
    if (array_val == irb->codegen->invalid_instruction)
        return array_val;

    IrInstruction *array_type = ir_build_typeof(irb, array_node, array_val);
    IrInstruction *pointer_type = ir_build_to_ptr_type(irb, array_node, array_type);
    IrInstruction *elem_var_type;
    if (node->data.for_expr.elem_is_ptr) {
        elem_var_type = pointer_type;
    } else {
        elem_var_type = ir_build_ptr_type_child(irb, elem_node, pointer_type);
    }
    bool is_inline = node->data.for_expr.is_inline;

    BlockContext *child_scope = new_block_context(node, parent_scope);
    child_scope->parent_loop_node = node;
    elem_node->block_context = child_scope;

    // TODO make it an error to write to element variable or i variable.
    Buf *elem_var_name = elem_node->data.symbol_expr.symbol;
    node->data.for_expr.elem_var = ir_add_local_var(irb, elem_node, child_scope, elem_var_name,
            true, false, false, is_inline);
    IrInstruction *undefined_value = ir_build_const_undefined(irb, elem_node);
    ir_build_var_decl(irb, elem_node, node->data.for_expr.elem_var, elem_var_type, undefined_value); 
    IrInstruction *elem_var_ptr = ir_build_var_ptr(irb, node, node->data.for_expr.elem_var);

    AstNode *index_var_source_node;
    if (index_node) {
        index_var_source_node = index_node;
        Buf *index_var_name = index_node->data.symbol_expr.symbol;
        index_node->block_context = child_scope;
        node->data.for_expr.index_var = ir_add_local_var(irb, index_node, child_scope, index_var_name,
                true, false, false, is_inline);
    } else {
        index_var_source_node = node;
        node->data.for_expr.index_var = ir_add_local_var(irb, node, child_scope, nullptr,
                true, false, true, is_inline);
    }
    IrInstruction *usize = ir_build_const_type(irb, node, irb->codegen->builtin_types.entry_usize);
    IrInstruction *zero = ir_build_const_usize(irb, node, 0);
    IrInstruction *one = ir_build_const_usize(irb, node, 1);
    ir_build_var_decl(irb, index_var_source_node, node->data.for_expr.index_var, usize, zero); 
    IrInstruction *index_ptr = ir_build_var_ptr(irb, node, node->data.for_expr.index_var);


    IrBasicBlock *cond_block = ir_build_basic_block(irb, "ForCond");
    IrBasicBlock *body_block = ir_build_basic_block(irb, "ForBody");
    IrBasicBlock *end_block = ir_build_basic_block(irb, "ForEnd");
    IrBasicBlock *continue_block = ir_build_basic_block(irb, "ForContinue");

    IrInstruction *len_val = ir_build_read_field(irb, node, array_val, irb->codegen->len_buf);
    ir_build_br(irb, node, cond_block, is_inline);

    ir_set_cursor_at_end(irb, cond_block);
    IrInstruction *index_val = ir_build_load_ptr(irb, node, index_ptr);
    IrInstruction *cond = ir_build_bin_op(irb, node, IrBinOpCmpLessThan, index_val, len_val);
    ir_build_cond_br(irb, node, cond, body_block, end_block, is_inline);

    ir_set_cursor_at_end(irb, body_block);
    IrInstruction *elem_ptr = ir_build_elem_ptr(irb, node, array_val, index_val);
    IrInstruction *elem_val;
    if (node->data.for_expr.elem_is_ptr) {
        elem_val = elem_ptr;
    } else {
        elem_val = ir_build_load_ptr(irb, node, elem_ptr);
    }
    ir_build_store_ptr(irb, node, elem_var_ptr, elem_val);

    irb->break_block_stack.append(end_block);
    irb->continue_block_stack.append(continue_block);
    ir_gen_node(irb, body_node, child_scope);
    irb->break_block_stack.pop();
    irb->continue_block_stack.pop();

    ir_build_br(irb, node, continue_block, is_inline);

    ir_set_cursor_at_end(irb, continue_block);
    IrInstruction *new_index_val = ir_build_bin_op(irb, node, IrBinOpAdd, index_val, one);
    ir_build_store_ptr(irb, node, index_ptr, new_index_val);
    ir_build_br(irb, node, cond_block, is_inline);

    ir_set_cursor_at_end(irb, end_block);
    return ir_build_const_void(irb, node);

}

static IrInstruction *ir_gen_this_literal(IrBuilder *irb, AstNode *node) {
    assert(node->type == NodeTypeThisLiteral);

    BlockContext *scope = node->block_context;

    if (!scope->parent)
        return ir_build_const_import(irb, node, node->owner);

    if (scope->fn_entry && (!scope->parent->fn_entry ||
        (scope->parent->parent && !scope->parent->parent->fn_entry)))
    {
        return ir_build_const_fn(irb, node, scope->fn_entry);
    }

    if (scope->node->type == NodeTypeContainerDecl) {
        TypeTableEntry *container_type = scope->node->data.struct_decl.type_entry;
        assert(container_type);
        return ir_build_const_type(irb, node, container_type);
    }

    if (scope->node->type == NodeTypeBlock)
        return ir_build_const_scope(irb, node, scope);

    zig_unreachable();
}

static IrInstruction *ir_gen_bool_literal(IrBuilder *irb, AstNode *node) {
    assert(node->type == NodeTypeBoolLiteral);
    return ir_build_const_bool(irb, node, node->data.bool_literal.value);
}

static IrInstruction *ir_gen_array_type(IrBuilder *irb, AstNode *node) {
    assert(node->type == NodeTypeArrayType);

    AstNode *size_node = node->data.array_type.size;
    AstNode *child_type_node = node->data.array_type.child_type;
    bool is_const = node->data.array_type.is_const;

    if (size_node) {
        if (is_const) {
            add_node_error(irb->codegen, node, buf_create_from_str("const qualifier invalid on array type"));
            return irb->codegen->invalid_instruction;
        }

        IrInstruction *size_value = ir_gen_node(irb, size_node, node->block_context);
        if (size_value == irb->codegen->invalid_instruction)
            return size_value;

        IrInstruction *child_type = ir_gen_node(irb, child_type_node, node->block_context);
        if (child_type == irb->codegen->invalid_instruction)
            return child_type;

        return ir_build_array_type(irb, node, size_value, child_type);
    } else {
        IrInstruction *child_type = ir_gen_node_extra(irb, child_type_node,
                node->block_context, LValPurposeAddressOf);
        if (child_type == irb->codegen->invalid_instruction)
            return child_type;

        return ir_build_slice_type(irb, node, is_const, child_type);
    }
}

static IrInstruction *ir_gen_node_extra(IrBuilder *irb, AstNode *node, BlockContext *block_context,
        LValPurpose lval)
{
    assert(block_context);
    node->block_context = block_context;

    switch (node->type) {
        case NodeTypeBlock:
            return ir_gen_block(irb, node);
        case NodeTypeBinOpExpr:
            return ir_gen_bin_op(irb, node);
        case NodeTypeNumberLiteral:
            return ir_gen_num_lit(irb, node);
        case NodeTypeSymbol:
            return ir_gen_symbol(irb, node, lval);
        case NodeTypeFnCallExpr:
            return ir_gen_fn_call(irb, node);
        case NodeTypeIfBoolExpr:
            return ir_gen_if_bool_expr(irb, node);
        case NodeTypePrefixOpExpr:
            return ir_gen_prefix_op_expr(irb, node);
        case NodeTypeContainerInitExpr:
            return ir_gen_container_init_expr(irb, node);
        case NodeTypeVariableDeclaration:
            return ir_gen_var_decl(irb, node);
        case NodeTypeWhileExpr:
            return ir_gen_while_expr(irb, node);
        case NodeTypeForExpr:
            return ir_gen_for_expr(irb, node);
        case NodeTypeArrayAccessExpr:
            return ir_gen_array_access(irb, node, lval);
        case NodeTypeReturnExpr:
            return ir_gen_return(irb, node);
        case NodeTypeFieldAccessExpr:
            return ir_gen_field_access(irb, node, lval);
        case NodeTypeThisLiteral:
            return ir_gen_this_literal(irb, node);
        case NodeTypeBoolLiteral:
            return ir_gen_bool_literal(irb, node);
        case NodeTypeArrayType:
            return ir_gen_array_type(irb, node);
        case NodeTypeUnwrapErrorExpr:
        case NodeTypeDefer:
        case NodeTypeSliceExpr:
        case NodeTypeIfVarExpr:
        case NodeTypeAsmExpr:
        case NodeTypeGoto:
        case NodeTypeBreak:
        case NodeTypeContinue:
        case NodeTypeLabel:
        case NodeTypeSwitchExpr:
        case NodeTypeStringLiteral:
        case NodeTypeCharLiteral:
        case NodeTypeNullLiteral:
        case NodeTypeUndefinedLiteral:
        case NodeTypeZeroesLiteral:
        case NodeTypeErrorType:
        case NodeTypeTypeLiteral:
        case NodeTypeVarLiteral:
        case NodeTypeRoot:
        case NodeTypeFnProto:
        case NodeTypeFnDef:
        case NodeTypeFnDecl:
        case NodeTypeParamDecl:
        case NodeTypeUse:
        case NodeTypeContainerDecl:
        case NodeTypeStructField:
        case NodeTypeStructValueField:
        case NodeTypeSwitchProng:
        case NodeTypeSwitchRange:
        case NodeTypeErrorValueDecl:
        case NodeTypeTypeDecl:
            zig_panic("TODO more IR gen");
    }
    zig_unreachable();
}

static IrInstruction *ir_gen_node(IrBuilder *irb, AstNode *node, BlockContext *scope) {
    return ir_gen_node_extra(irb, node, scope, LValPurposeNone);
}

IrInstruction *ir_gen(CodeGen *codegen, AstNode *node, BlockContext *scope, IrExecutable *ir_executable) {
    assert(node->owner);

    IrBuilder ir_gen = {0};
    IrBuilder *irb = &ir_gen;

    irb->codegen = codegen;
    irb->exec = ir_executable;

    irb->current_basic_block = ir_build_basic_block(irb, "Entry");
    // Entry block gets a reference because we enter it to begin.
    ir_ref_bb(irb->current_basic_block);

    IrInstruction *result = ir_gen_node_extra(irb, node, scope, LValPurposeNone);
    assert(result);

    if (result == codegen->invalid_instruction)
        return result;

    return ir_build_return(irb, result->source_node, result);
}

IrInstruction *ir_gen_fn(CodeGen *codegn, FnTableEntry *fn_entry) {
    assert(fn_entry);

    IrExecutable *ir_executable = &fn_entry->ir_executable;
    AstNode *fn_def_node = fn_entry->fn_def_node;
    assert(fn_def_node->type == NodeTypeFnDef);

    AstNode *body_node = fn_def_node->data.fn_def.body;
    BlockContext *scope = fn_def_node->data.fn_def.block_context;

    return ir_gen(codegn, body_node, scope, ir_executable);
}

static bool ir_num_lit_fits_in_other_type(IrAnalyze *ira, IrInstruction *instruction, TypeTableEntry *other_type) {
    TypeTableEntry *other_type_underlying = get_underlying_type(other_type);

    if (other_type_underlying->id == TypeTableEntryIdInvalid) {
        return false;
    }

    ConstExprValue *const_val = &instruction->static_value;
    assert(const_val->ok);
    if (other_type_underlying->id == TypeTableEntryIdFloat) {
        return true;
    } else if (other_type_underlying->id == TypeTableEntryIdInt &&
               const_val->data.x_bignum.kind == BigNumKindInt)
    {
        if (bignum_fits_in_bits(&const_val->data.x_bignum, other_type_underlying->data.integral.bit_count,
                    other_type_underlying->data.integral.is_signed))
        {
            return true;
        }
    } else if ((other_type_underlying->id == TypeTableEntryIdNumLitFloat &&
                const_val->data.x_bignum.kind == BigNumKindFloat) ||
               (other_type_underlying->id == TypeTableEntryIdNumLitInt &&
                const_val->data.x_bignum.kind == BigNumKindInt))
    {
        return true;
    }

    const char *num_lit_str = (const_val->data.x_bignum.kind == BigNumKindFloat) ? "float" : "integer";

    add_node_error(ira->codegen, instruction->source_node,
        buf_sprintf("%s value %s cannot be implicitly casted to type '%s'",
            num_lit_str,
            buf_ptr(bignum_to_buf(&const_val->data.x_bignum)),
            buf_ptr(&other_type->name)));
    return false;
}

static TypeTableEntry *ir_determine_peer_types(IrAnalyze *ira, AstNode *source_node,
        IrInstruction **instructions, size_t instruction_count)
{
    assert(instruction_count >= 1);
    IrInstruction *prev_inst = instructions[0];
    if (prev_inst->type_entry->id == TypeTableEntryIdInvalid) {
        return ira->codegen->builtin_types.entry_invalid;
    }
    for (size_t i = 1; i < instruction_count; i += 1) {
        IrInstruction *cur_inst = instructions[i];
        TypeTableEntry *cur_type = cur_inst->type_entry;
        TypeTableEntry *prev_type = prev_inst->type_entry;
        if (cur_type->id == TypeTableEntryIdInvalid) {
            return cur_type;
        } else if (types_match_const_cast_only(prev_type, cur_type)) {
            continue;
        } else if (types_match_const_cast_only(cur_type, prev_type)) {
            prev_inst = cur_inst;
            continue;
        } else if (prev_type->id == TypeTableEntryIdUnreachable) {
            prev_inst = cur_inst;
        } else if (cur_type->id == TypeTableEntryIdUnreachable) {
            continue;
        } else if (prev_type->id == TypeTableEntryIdInt &&
                   cur_type->id == TypeTableEntryIdInt &&
                   prev_type->data.integral.is_signed == cur_type->data.integral.is_signed)
        {
            if (cur_type->data.integral.bit_count > prev_type->data.integral.bit_count) {
                prev_inst = cur_inst;
            }
            continue;
        } else if (prev_type->id == TypeTableEntryIdFloat &&
                   cur_type->id == TypeTableEntryIdFloat)
        {
            if (cur_type->data.floating.bit_count > prev_type->data.floating.bit_count) {
                prev_inst = cur_inst;
            }
        } else if (prev_type->id == TypeTableEntryIdErrorUnion &&
                   types_match_const_cast_only(prev_type->data.error.child_type, cur_type))
        {
            continue;
        } else if (cur_type->id == TypeTableEntryIdErrorUnion &&
                   types_match_const_cast_only(cur_type->data.error.child_type, prev_type))
        {
            prev_inst = cur_inst;
            continue;
        } else if (prev_type->id == TypeTableEntryIdNumLitInt ||
                    prev_type->id == TypeTableEntryIdNumLitFloat)
        {
            if (ir_num_lit_fits_in_other_type(ira, prev_inst, cur_type)) {
                prev_inst = cur_inst;
                continue;
            } else {
                return ira->codegen->builtin_types.entry_invalid;
            }
        } else if (cur_type->id == TypeTableEntryIdNumLitInt ||
                   cur_type->id == TypeTableEntryIdNumLitFloat)
        {
            if (ir_num_lit_fits_in_other_type(ira, cur_inst, prev_type)) {
                continue;
            } else {
                return ira->codegen->builtin_types.entry_invalid;
            }
        } else {
            add_node_error(ira->codegen, source_node,
                buf_sprintf("incompatible types: '%s' and '%s'",
                    buf_ptr(&prev_type->name), buf_ptr(&cur_type->name)));

            return ira->codegen->builtin_types.entry_invalid;
        }
    }
    return prev_inst->type_entry;
}

enum ImplicitCastMatchResult {
    ImplicitCastMatchResultNo,
    ImplicitCastMatchResultYes,
    ImplicitCastMatchResultReportedError,
};

static ImplicitCastMatchResult ir_types_match_with_implicit_cast(IrAnalyze *ira, TypeTableEntry *expected_type,
        TypeTableEntry *actual_type, IrInstruction *value)
{
    if (types_match_const_cast_only(expected_type, actual_type)) {
        return ImplicitCastMatchResultYes;
    }

    // implicit conversion from non maybe type to maybe type
    if (expected_type->id == TypeTableEntryIdMaybe &&
        ir_types_match_with_implicit_cast(ira, expected_type->data.maybe.child_type, actual_type, value))
    {
        return ImplicitCastMatchResultYes;
    }

    // implicit conversion from null literal to maybe type
    if (expected_type->id == TypeTableEntryIdMaybe &&
        actual_type->id == TypeTableEntryIdNullLit)
    {
        return ImplicitCastMatchResultYes;
    }

    // implicit conversion from error child type to error type
    if (expected_type->id == TypeTableEntryIdErrorUnion &&
        ir_types_match_with_implicit_cast(ira, expected_type->data.error.child_type, actual_type, value))
    {
        return ImplicitCastMatchResultYes;
    }

    // implicit conversion from pure error to error union type
    if (expected_type->id == TypeTableEntryIdErrorUnion &&
        actual_type->id == TypeTableEntryIdPureError)
    {
        return ImplicitCastMatchResultYes;
    }

    // implicit widening conversion
    if (expected_type->id == TypeTableEntryIdInt &&
        actual_type->id == TypeTableEntryIdInt &&
        expected_type->data.integral.is_signed == actual_type->data.integral.is_signed &&
        expected_type->data.integral.bit_count >= actual_type->data.integral.bit_count)
    {
        return ImplicitCastMatchResultYes;
    }

    // small enough unsigned ints can get casted to large enough signed ints
    if (expected_type->id == TypeTableEntryIdInt && expected_type->data.integral.is_signed &&
        actual_type->id == TypeTableEntryIdInt && !actual_type->data.integral.is_signed &&
        expected_type->data.integral.bit_count > actual_type->data.integral.bit_count)
    {
        return ImplicitCastMatchResultYes;
    }

    // implicit float widening conversion
    if (expected_type->id == TypeTableEntryIdFloat &&
        actual_type->id == TypeTableEntryIdFloat &&
        expected_type->data.floating.bit_count >= actual_type->data.floating.bit_count)
    {
        return ImplicitCastMatchResultYes;
    }

    // implicit array to slice conversion
    if (expected_type->id == TypeTableEntryIdStruct &&
        expected_type->data.structure.is_slice &&
        actual_type->id == TypeTableEntryIdArray &&
        types_match_const_cast_only(
            expected_type->data.structure.fields[0].type_entry->data.pointer.child_type,
            actual_type->data.array.child_type))
    {
        return ImplicitCastMatchResultYes;
    }

    // implicit number literal to typed number
    if ((actual_type->id == TypeTableEntryIdNumLitFloat ||
         actual_type->id == TypeTableEntryIdNumLitInt))
    {
        if (ir_num_lit_fits_in_other_type(ira, value, expected_type)) {
            return ImplicitCastMatchResultYes;
        } else {
            return ImplicitCastMatchResultReportedError;
        }
    }

    // implicit undefined literal to anything
    if (actual_type->id == TypeTableEntryIdUndefLit) {
        return ImplicitCastMatchResultYes;
    }


    return ImplicitCastMatchResultNo;
}

static TypeTableEntry *ir_resolve_peer_types(IrAnalyze *ira, AstNode *source_node,
        IrInstruction **instructions, size_t instruction_count)
{
    return ir_determine_peer_types(ira, source_node, instructions, instruction_count);
}

static IrInstruction *ir_resolve_cast(IrAnalyze *ira, IrInstruction *source_instr, IrInstruction *value,
        IrInstruction *dest_type, CastOp cast_op, bool need_alloca)
{
    assert(dest_type->type_entry->id == TypeTableEntryIdMetaType);
    assert(dest_type->static_value.ok);
    TypeTableEntry *wanted_type = dest_type->static_value.data.x_type;

    if (value->static_value.ok) {
        IrInstruction *result = ir_create_const(&ira->new_irb, source_instr->source_node, wanted_type);
        eval_const_expr_implicit_cast(cast_op, &value->static_value, value->type_entry,
                &result->static_value, wanted_type);
        return result;
    } else {
        IrInstruction *result = ir_build_cast(&ira->new_irb, source_instr->source_node,
                dest_type->other, value->other, cast_op);
        result->type_entry = wanted_type;
        if (need_alloca && source_instr->source_node->block_context->fn_entry) {
            IrInstructionCast *cast_instruction = (IrInstructionCast *)result;
            source_instr->source_node->block_context->fn_entry->cast_alloca_list.append(cast_instruction);
        }
        return result;
    }
}

static bool is_slice(TypeTableEntry *type) {
    return type->id == TypeTableEntryIdStruct && type->data.structure.is_slice;
}

static bool is_u8(TypeTableEntry *type) {
    return type->id == TypeTableEntryIdInt &&
        !type->data.integral.is_signed && type->data.integral.bit_count == 8;
}

static IrBasicBlock *ir_get_new_bb(IrAnalyze *ira, IrBasicBlock *old_bb) {
    if (old_bb->other)
        return old_bb->other;
    IrBasicBlock *new_bb = ir_build_bb_from(&ira->new_irb, old_bb);
    ira->old_bb_queue.append(old_bb);
    return new_bb;
}

static void ir_finish_bb(IrAnalyze *ira) {
    ira->block_queue_index += 1;

    if (ira->block_queue_index < ira->old_bb_queue.length) {
        IrBasicBlock *old_bb = ira->old_bb_queue.at(ira->block_queue_index);
        ira->instruction_index = 0;
        ira->new_irb.current_basic_block = ir_get_new_bb(ira, old_bb);
        ira->old_irb.current_basic_block = old_bb;
        ira->const_predecessor_bb = nullptr;
    }
}

static void ir_inline_bb(IrAnalyze *ira, IrBasicBlock *old_bb) {
    ira->instruction_index = 0;
    ira->const_predecessor_bb = ira->old_irb.current_basic_block;
    ira->old_irb.current_basic_block = old_bb;
}


static ConstExprValue *ir_build_const_from(IrAnalyze *ira, IrInstruction *old_instruction,
        bool depends_on_compile_var)
{
    IrInstruction *new_instruction;
    if (old_instruction->id == IrInstructionIdVarPtr) {
        IrInstructionVarPtr *old_var_ptr_instruction = (IrInstructionVarPtr *)old_instruction;
        IrInstructionVarPtr *var_ptr_instruction = ir_create_instruction<IrInstructionVarPtr>(ira->new_irb.exec,
                old_instruction->source_node);
        var_ptr_instruction->var = old_var_ptr_instruction->var;
        new_instruction = &var_ptr_instruction->base;
    } else if (old_instruction->id == IrInstructionIdFieldPtr) {
        zig_panic("TODO");
    } else if (old_instruction->id == IrInstructionIdElemPtr) {
        zig_panic("TODO");
    } else {
        IrInstructionConst *const_instruction = ir_create_instruction<IrInstructionConst>(ira->new_irb.exec,
                old_instruction->source_node);
        new_instruction = &const_instruction->base;
    }
    ir_link_new_instruction(new_instruction, old_instruction);
    ConstExprValue *const_val = &new_instruction->static_value;
    const_val->ok = true;
    const_val->depends_on_compile_var = depends_on_compile_var;
    return const_val;
}

static TypeTableEntry *ir_analyze_void(IrAnalyze *ira, IrInstruction *instruction) {
    ir_build_const_from(ira, instruction, false);
    return ira->codegen->builtin_types.entry_void;
}

static TypeTableEntry *ir_analyze_const_usize(IrAnalyze *ira, IrInstruction *instruction, uint64_t value,
    bool depends_on_compile_var)
{
    ConstExprValue *const_val = ir_build_const_from(ira, instruction, depends_on_compile_var);
    bignum_init_unsigned(&const_val->data.x_bignum, value);
    return ira->codegen->builtin_types.entry_usize;
}

static TypeTableEntry *ir_resolve_type(IrAnalyze *ira, IrInstruction *type_value) {
    if (type_value == ira->codegen->invalid_instruction)
        return ira->codegen->builtin_types.entry_invalid;

    if (type_value->type_entry->id == TypeTableEntryIdInvalid)
        return ira->codegen->builtin_types.entry_invalid;

    if (type_value->type_entry->id != TypeTableEntryIdMetaType) {
        add_node_error(ira->codegen, type_value->source_node,
                buf_sprintf("expected type 'type', found '%s'", buf_ptr(&type_value->type_entry->name)));
        return ira->codegen->builtin_types.entry_invalid;
    }

    ConstExprValue *const_val = &type_value->static_value;
    if (!const_val->ok) {
        add_node_error(ira->codegen, type_value->source_node,
                buf_sprintf("unable to evaluate constant expression"));
        return ira->codegen->builtin_types.entry_invalid;
    }

    return const_val->data.x_type;
}

static bool ir_resolve_bool(IrAnalyze *ira, IrInstruction *bool_value, bool *out) {
    if (bool_value == ira->codegen->invalid_instruction)
        return false;

    if (bool_value->type_entry->id == TypeTableEntryIdInvalid)
        return false;

    if (bool_value->type_entry->id != TypeTableEntryIdBool) {
        add_node_error(ira->codegen, bool_value->source_node,
                buf_sprintf("expected type 'bool', found '%s'", buf_ptr(&bool_value->type_entry->name)));
        return false;
    }

    ConstExprValue *const_val = &bool_value->static_value;
    if (!const_val->ok) {
        add_node_error(ira->codegen, bool_value->source_node,
                buf_sprintf("unable to evaluate constant expression"));
        return false;
    }

    *out = const_val->data.x_bool;
    return true;
}

static FnTableEntry *ir_resolve_fn(IrAnalyze *ira, IrInstruction *fn_value) {
    if (fn_value == ira->codegen->invalid_instruction)
        return nullptr;

    if (fn_value->type_entry->id == TypeTableEntryIdInvalid)
        return nullptr;

    if (fn_value->type_entry->id != TypeTableEntryIdFn) {
        add_node_error(ira->codegen, fn_value->source_node,
                buf_sprintf("expected function type, found '%s'", buf_ptr(&fn_value->type_entry->name)));
        return nullptr;
    }

    ConstExprValue *const_val = &fn_value->static_value;
    if (!const_val->ok) {
        add_node_error(ira->codegen, fn_value->source_node,
                buf_sprintf("unable to evaluate constant expression"));
        return nullptr;
    }

    return const_val->data.x_fn;
}

static IrInstruction *ir_analyze_cast(IrAnalyze *ira, IrInstruction *source_instr,
    IrInstruction *dest_type, IrInstruction *value)
{
    assert(dest_type->type_entry->id == TypeTableEntryIdMetaType);
    assert(dest_type->static_value.ok);

    TypeTableEntry *wanted_type = dest_type->static_value.data.x_type;
    TypeTableEntry *actual_type = value->type_entry;
    TypeTableEntry *wanted_type_canon = get_underlying_type(wanted_type);
    TypeTableEntry *actual_type_canon = get_underlying_type(actual_type);

    TypeTableEntry *isize_type = ira->codegen->builtin_types.entry_isize;
    TypeTableEntry *usize_type = ira->codegen->builtin_types.entry_usize;

    if (wanted_type_canon->id == TypeTableEntryIdInvalid ||
        actual_type_canon->id == TypeTableEntryIdInvalid)
    {
        return ira->codegen->invalid_instruction;
    }

    // explicit match or non-const to const
    if (types_match_const_cast_only(wanted_type, actual_type)) {
        return ir_resolve_cast(ira, source_instr, value, dest_type, CastOpNoop, false);
    }

    // explicit cast from bool to int
    if (wanted_type_canon->id == TypeTableEntryIdInt &&
        actual_type_canon->id == TypeTableEntryIdBool)
    {
        return ir_resolve_cast(ira, source_instr, value, dest_type, CastOpBoolToInt, false);
    }

    // explicit cast from pointer to isize or usize
    if ((wanted_type_canon == isize_type || wanted_type_canon == usize_type) &&
        type_is_codegen_pointer(actual_type_canon))
    {
        return ir_resolve_cast(ira, source_instr, value, dest_type, CastOpPtrToInt, false);
    }


    // explicit cast from isize or usize to pointer
    if (wanted_type_canon->id == TypeTableEntryIdPointer &&
        (actual_type_canon == isize_type || actual_type_canon == usize_type))
    {
        return ir_resolve_cast(ira, source_instr, value, dest_type, CastOpIntToPtr, false);
    }

    // explicit widening or shortening cast
    if ((wanted_type_canon->id == TypeTableEntryIdInt &&
        actual_type_canon->id == TypeTableEntryIdInt) ||
        (wanted_type_canon->id == TypeTableEntryIdFloat &&
        actual_type_canon->id == TypeTableEntryIdFloat))
    {
        return ir_resolve_cast(ira, source_instr, value, dest_type, CastOpWidenOrShorten, false);
    }

    // explicit cast from int to float
    if (wanted_type_canon->id == TypeTableEntryIdFloat &&
        actual_type_canon->id == TypeTableEntryIdInt)
    {
        return ir_resolve_cast(ira, source_instr, value, dest_type, CastOpIntToFloat, false);
    }

    // explicit cast from float to int
    if (wanted_type_canon->id == TypeTableEntryIdInt &&
        actual_type_canon->id == TypeTableEntryIdFloat)
    {
        return ir_resolve_cast(ira, source_instr, value, dest_type, CastOpFloatToInt, false);
    }

    // explicit cast from array to slice
    if (is_slice(wanted_type) &&
        actual_type->id == TypeTableEntryIdArray &&
        types_match_const_cast_only(
            wanted_type->data.structure.fields[0].type_entry->data.pointer.child_type,
            actual_type->data.array.child_type))
    {
        return ir_resolve_cast(ira, source_instr, value, dest_type, CastOpToUnknownSizeArray, true);
    }

    // explicit cast from []T to []u8 or []u8 to []T
    if (is_slice(wanted_type) && is_slice(actual_type) &&
        (is_u8(wanted_type->data.structure.fields[0].type_entry->data.pointer.child_type) ||
        is_u8(actual_type->data.structure.fields[0].type_entry->data.pointer.child_type)) &&
        (wanted_type->data.structure.fields[0].type_entry->data.pointer.is_const ||
         !actual_type->data.structure.fields[0].type_entry->data.pointer.is_const))
    {
        mark_impure_fn(ira->codegen, source_instr->source_node->block_context, source_instr->source_node);
        return ir_resolve_cast(ira, source_instr, value, dest_type, CastOpResizeSlice, true);
    }

    // explicit cast from [N]u8 to []T
    if (is_slice(wanted_type) &&
        actual_type->id == TypeTableEntryIdArray &&
        is_u8(actual_type->data.array.child_type))
    {
        mark_impure_fn(ira->codegen, source_instr->source_node->block_context, source_instr->source_node);
        uint64_t child_type_size = type_size(ira->codegen,
                wanted_type->data.structure.fields[0].type_entry->data.pointer.child_type);
        if (actual_type->data.array.len % child_type_size == 0) {
            return ir_resolve_cast(ira, source_instr, value, dest_type, CastOpBytesToSlice, true);
        } else {
            add_node_error(ira->codegen, source_instr->source_node,
                    buf_sprintf("unable to convert %s to %s: size mismatch",
                        buf_ptr(&actual_type->name), buf_ptr(&wanted_type->name)));
            return ira->codegen->invalid_instruction;
        }
    }

    // explicit cast from pointer to another pointer
    if ((actual_type->id == TypeTableEntryIdPointer || actual_type->id == TypeTableEntryIdFn) &&
        (wanted_type->id == TypeTableEntryIdPointer || wanted_type->id == TypeTableEntryIdFn))
    {
        return ir_resolve_cast(ira, source_instr, value, dest_type, CastOpPointerReinterpret, false);
    }

    // explicit cast from maybe pointer to another maybe pointer
    if (actual_type->id == TypeTableEntryIdMaybe &&
        (actual_type->data.maybe.child_type->id == TypeTableEntryIdPointer ||
            actual_type->data.maybe.child_type->id == TypeTableEntryIdFn) &&
        wanted_type->id == TypeTableEntryIdMaybe &&
        (wanted_type->data.maybe.child_type->id == TypeTableEntryIdPointer ||
            wanted_type->data.maybe.child_type->id == TypeTableEntryIdFn))
    {
        return ir_resolve_cast(ira, source_instr, value, dest_type, CastOpPointerReinterpret, false);
    }

    // explicit cast from child type of maybe type to maybe type
    if (wanted_type->id == TypeTableEntryIdMaybe) {
        if (types_match_const_cast_only(wanted_type->data.maybe.child_type, actual_type)) {
            IrInstruction *cast_instruction = ir_resolve_cast(ira, source_instr, value, dest_type,
                    CastOpMaybeWrap, true);
            cast_instruction->return_knowledge = ReturnKnowledgeKnownNonNull;
            return cast_instruction;
        } else if (actual_type->id == TypeTableEntryIdNumLitInt ||
                   actual_type->id == TypeTableEntryIdNumLitFloat)
        {
            if (ir_num_lit_fits_in_other_type(ira, value, wanted_type->data.maybe.child_type)) {
                IrInstruction *cast_instruction = ir_resolve_cast(ira, source_instr, value, dest_type,
                        CastOpMaybeWrap, true);
                cast_instruction->return_knowledge = ReturnKnowledgeKnownNonNull;
                return cast_instruction;
            } else {
                return ira->codegen->invalid_instruction;
            }
        }
    }

    // explicit cast from null literal to maybe type
    if (wanted_type->id == TypeTableEntryIdMaybe &&
        actual_type->id == TypeTableEntryIdNullLit)
    {
        IrInstruction *cast_instruction = ir_resolve_cast(ira, source_instr, value, dest_type,
                CastOpNullToMaybe, true);
        cast_instruction->return_knowledge = ReturnKnowledgeKnownNull;
        return cast_instruction;
    }

    // explicit cast from child type of error type to error type
    if (wanted_type->id == TypeTableEntryIdErrorUnion) {
        if (types_match_const_cast_only(wanted_type->data.error.child_type, actual_type)) {
            IrInstruction *cast_instruction = ir_resolve_cast(ira, source_instr, value, dest_type,
                    CastOpErrorWrap, true);
            cast_instruction->return_knowledge = ReturnKnowledgeKnownNonError;
            return cast_instruction;
        } else if (actual_type->id == TypeTableEntryIdNumLitInt ||
                   actual_type->id == TypeTableEntryIdNumLitFloat)
        {
            if (ir_num_lit_fits_in_other_type(ira, value, wanted_type->data.error.child_type)) {
                IrInstruction *cast_instruction = ir_resolve_cast(ira, source_instr, value, dest_type,
                        CastOpErrorWrap, true);
                cast_instruction->return_knowledge = ReturnKnowledgeKnownNonError;
                return cast_instruction;
            } else {
                return ira->codegen->invalid_instruction;
            }
        }
    }

    // explicit cast from pure error to error union type
    if (wanted_type->id == TypeTableEntryIdErrorUnion &&
        actual_type->id == TypeTableEntryIdPureError)
    {
        IrInstruction *cast_instruction = ir_resolve_cast(ira, source_instr, value, dest_type,
                CastOpPureErrorWrap, false);
        cast_instruction->return_knowledge = ReturnKnowledgeKnownError;
        return cast_instruction;
    }

    // explicit cast from number literal to another type
    if (actual_type->id == TypeTableEntryIdNumLitFloat ||
        actual_type->id == TypeTableEntryIdNumLitInt)
    {
        if (ir_num_lit_fits_in_other_type(ira, value, wanted_type_canon)) {
            CastOp op;
            if ((actual_type->id == TypeTableEntryIdNumLitFloat &&
                 wanted_type_canon->id == TypeTableEntryIdFloat) ||
                (actual_type->id == TypeTableEntryIdNumLitInt &&
                 wanted_type_canon->id == TypeTableEntryIdInt))
            {
                op = CastOpNoop;
            } else if (wanted_type_canon->id == TypeTableEntryIdInt) {
                op = CastOpFloatToInt;
            } else if (wanted_type_canon->id == TypeTableEntryIdFloat) {
                op = CastOpIntToFloat;
            } else {
                zig_unreachable();
            }
            return ir_resolve_cast(ira, source_instr, value, dest_type, op, false);
        } else {
            return ira->codegen->invalid_instruction;
        }
    }

    // explicit cast from %void to integer type which can fit it
    bool actual_type_is_void_err = actual_type->id == TypeTableEntryIdErrorUnion &&
        !type_has_bits(actual_type->data.error.child_type);
    bool actual_type_is_pure_err = actual_type->id == TypeTableEntryIdPureError;
    if ((actual_type_is_void_err || actual_type_is_pure_err) &&
        wanted_type->id == TypeTableEntryIdInt)
    {
        BigNum bn;
        bignum_init_unsigned(&bn, ira->codegen->error_decls.length);
        if (bignum_fits_in_bits(&bn, wanted_type->data.integral.bit_count,
                    wanted_type->data.integral.is_signed))
        {
            return ir_resolve_cast(ira, source_instr, value, dest_type, CastOpErrToInt, false);
        } else {
            add_node_error(ira->codegen, source_instr->source_node,
                    buf_sprintf("too many error values to fit in '%s'", buf_ptr(&wanted_type->name)));
            return ira->codegen->invalid_instruction;
        }
    }

    // explicit cast from integer to enum type with no payload
    if (actual_type->id == TypeTableEntryIdInt &&
        wanted_type->id == TypeTableEntryIdEnum &&
        wanted_type->data.enumeration.gen_field_count == 0)
    {
        return ir_resolve_cast(ira, source_instr, value, dest_type, CastOpIntToEnum, false);
    }

    // explicit cast from enum type with no payload to integer
    if (wanted_type->id == TypeTableEntryIdInt &&
        actual_type->id == TypeTableEntryIdEnum &&
        actual_type->data.enumeration.gen_field_count == 0)
    {
        return ir_resolve_cast(ira, source_instr, value, dest_type, CastOpEnumToInt, false);
    }

    // explicit cast from undefined to anything
    if (actual_type->id == TypeTableEntryIdUndefLit) {
        return ir_resolve_cast(ira, source_instr, value, dest_type, CastOpNoop, false);
    }

    add_node_error(ira->codegen, source_instr->source_node,
        buf_sprintf("invalid cast from type '%s' to '%s'",
            buf_ptr(&actual_type->name),
            buf_ptr(&wanted_type->name)));
    return ira->codegen->invalid_instruction;
}

static IrInstruction *ir_get_casted_value(IrAnalyze *ira, IrInstruction *value, TypeTableEntry *expected_type) {
    assert(value);
    assert(value != ira->codegen->invalid_instruction);
    assert(!expected_type || expected_type->id != TypeTableEntryIdInvalid);
    assert(value->type_entry);
    assert(value->type_entry->id != TypeTableEntryIdInvalid);
    if (expected_type == nullptr)
        return value; // anything will do
    if (expected_type == value->type_entry)
        return value; // match
    if (value->type_entry->id == TypeTableEntryIdUnreachable)
        return value;

    ImplicitCastMatchResult result = ir_types_match_with_implicit_cast(ira, expected_type, value->type_entry, value);
    switch (result) {
        case ImplicitCastMatchResultNo:
            add_node_error(ira->codegen, first_executing_node(value->source_node),
                buf_sprintf("expected type '%s', got '%s'",
                    buf_ptr(&expected_type->name),
                    buf_ptr(&value->type_entry->name)));
            return ira->codegen->invalid_instruction;

        case ImplicitCastMatchResultYes:
            {
                IrInstruction *dest_type = ir_create_const_type(&ira->new_irb, value->source_node, expected_type);
                IrInstruction *cast_instruction = ir_analyze_cast(ira, value, dest_type, value);
                return cast_instruction;
            }
        case ImplicitCastMatchResultReportedError:
            return ira->codegen->invalid_instruction;
    }

    zig_unreachable();
}

static TypeTableEntry *ir_analyze_instruction_return(IrAnalyze *ira,
    IrInstructionReturn *return_instruction)
{
    IrInstruction *value = return_instruction->value->other;
    if (value == ira->codegen->invalid_instruction) {
        ir_finish_bb(ira);
        return ira->codegen->builtin_types.entry_unreachable;
    }
    ira->implicit_return_type_list.append(value);

    IrInstruction *casted_value = ir_get_casted_value(ira, value, ira->explicit_return_type);
    if (casted_value == ira->codegen->invalid_instruction) {
        ir_finish_bb(ira);
        return ira->codegen->builtin_types.entry_unreachable;
    }

    ir_build_return_from(&ira->new_irb, &return_instruction->base, casted_value);
    ir_finish_bb(ira);
    return ira->codegen->builtin_types.entry_unreachable;
}

static TypeTableEntry *ir_analyze_instruction_const(IrAnalyze *ira, IrInstructionConst *const_instruction) {
    const_instruction->base.other = &const_instruction->base;
    return const_instruction->base.type_entry;
}

static TypeTableEntry *ir_analyze_bin_op_bool(IrAnalyze *ira, IrInstructionBinOp *bin_op_instruction) {
    IrInstruction *op1 = bin_op_instruction->op1;
    IrInstruction *op2 = bin_op_instruction->op2;

    TypeTableEntry *bool_type = ira->codegen->builtin_types.entry_bool;

    IrInstruction *casted_op1 = ir_get_casted_value(ira, op1->other, bool_type);
    if (casted_op1 == ira->codegen->invalid_instruction)
        return ira->codegen->builtin_types.entry_invalid;

    IrInstruction *casted_op2 = ir_get_casted_value(ira, op2->other, bool_type);
    if (casted_op2 == ira->codegen->invalid_instruction)
        return ira->codegen->builtin_types.entry_invalid;

    ConstExprValue *op1_val = &casted_op1->static_value;
    ConstExprValue *op2_val = &casted_op2->static_value;
    if (op1_val->ok && op2_val->ok) {
        bool depends_on_compile_var = op1_val->depends_on_compile_var || op2_val->depends_on_compile_var;
        ConstExprValue *out_val = ir_build_const_from(ira, &bin_op_instruction->base, depends_on_compile_var);

        assert(op1->type_entry->id == TypeTableEntryIdBool);
        assert(op2->type_entry->id == TypeTableEntryIdBool);
        if (bin_op_instruction->op_id == IrBinOpBoolOr) {
            out_val->data.x_bool = op1_val->data.x_bool || op2_val->data.x_bool;
        } else if (bin_op_instruction->op_id == IrBinOpBoolAnd) {
            out_val->data.x_bool = op1_val->data.x_bool && op2_val->data.x_bool;
        } else {
            zig_unreachable();
        }
        return bool_type;
    }

    ir_build_bin_op_from(&ira->new_irb, &bin_op_instruction->base, bin_op_instruction->op_id, op1->other, op2->other);
    return bool_type;
}

static TypeTableEntry *ir_analyze_bin_op_cmp(IrAnalyze *ira, IrInstructionBinOp *bin_op_instruction) {
    IrInstruction *op1 = bin_op_instruction->op1->other;
    IrInstruction *op2 = bin_op_instruction->op2->other;
    IrInstruction *instructions[] = {op1, op2};
    TypeTableEntry *resolved_type = ir_resolve_peer_types(ira, bin_op_instruction->base.source_node, instructions, 2);
    if (resolved_type->id == TypeTableEntryIdInvalid)
        return resolved_type;
    IrBinOp op_id = bin_op_instruction->op_id;

    bool is_equality_cmp = (op_id == IrBinOpCmpEq || op_id == IrBinOpCmpNotEq);
    AstNode *source_node = bin_op_instruction->base.source_node;
    switch (resolved_type->id) {
        case TypeTableEntryIdInvalid:
            return ira->codegen->builtin_types.entry_invalid;

        case TypeTableEntryIdNumLitFloat:
        case TypeTableEntryIdNumLitInt:
        case TypeTableEntryIdInt:
        case TypeTableEntryIdFloat:
            break;

        case TypeTableEntryIdBool:
        case TypeTableEntryIdMetaType:
        case TypeTableEntryIdVoid:
        case TypeTableEntryIdPointer:
        case TypeTableEntryIdPureError:
        case TypeTableEntryIdFn:
        case TypeTableEntryIdTypeDecl:
        case TypeTableEntryIdNamespace:
        case TypeTableEntryIdBlock:
        case TypeTableEntryIdGenericFn:
            if (!is_equality_cmp) {
                add_node_error(ira->codegen, source_node,
                    buf_sprintf("operator not allowed for type '%s'", buf_ptr(&resolved_type->name)));
                return ira->codegen->builtin_types.entry_invalid;
            }
            break;

        case TypeTableEntryIdEnum:
            if (!is_equality_cmp || resolved_type->data.enumeration.gen_field_count != 0) {
                add_node_error(ira->codegen, source_node,
                    buf_sprintf("operator not allowed for type '%s'", buf_ptr(&resolved_type->name)));
                return ira->codegen->builtin_types.entry_invalid;
            }
            break;

        case TypeTableEntryIdUnreachable:
        case TypeTableEntryIdArray:
        case TypeTableEntryIdStruct:
        case TypeTableEntryIdUndefLit:
        case TypeTableEntryIdNullLit:
        case TypeTableEntryIdMaybe:
        case TypeTableEntryIdErrorUnion:
        case TypeTableEntryIdUnion:
            add_node_error(ira->codegen, source_node,
                buf_sprintf("operator not allowed for type '%s'", buf_ptr(&resolved_type->name)));
            return ira->codegen->builtin_types.entry_invalid;

        case TypeTableEntryIdVar:
            zig_unreachable();
    }

    IrInstruction *casted_op1 = ir_get_casted_value(ira, op1, resolved_type);
    if (casted_op1 == ira->codegen->invalid_instruction)
        return ira->codegen->builtin_types.entry_invalid;

    IrInstruction *casted_op2 = ir_get_casted_value(ira, op2, resolved_type);
    if (casted_op2 == ira->codegen->invalid_instruction)
        return ira->codegen->builtin_types.entry_invalid;

    ConstExprValue *op1_val = &casted_op1->static_value;
    ConstExprValue *op2_val = &casted_op2->static_value;
    if (op1_val->ok && op2_val->ok) {
        bool type_can_gt_lt_cmp = (resolved_type->id == TypeTableEntryIdNumLitFloat ||
                resolved_type->id == TypeTableEntryIdNumLitInt ||
                resolved_type->id == TypeTableEntryIdFloat ||
                resolved_type->id == TypeTableEntryIdInt);
        bool answer;
        if (type_can_gt_lt_cmp) {
            bool (*bignum_cmp)(BigNum *, BigNum *);
            if (op_id == IrBinOpCmpEq) {
                bignum_cmp = bignum_cmp_eq;
            } else if (op_id == IrBinOpCmpNotEq) {
                bignum_cmp = bignum_cmp_neq;
            } else if (op_id == IrBinOpCmpLessThan) {
                bignum_cmp = bignum_cmp_lt;
            } else if (op_id == IrBinOpCmpGreaterThan) {
                bignum_cmp = bignum_cmp_gt;
            } else if (op_id == IrBinOpCmpLessOrEq) {
                bignum_cmp = bignum_cmp_lte;
            } else if (op_id == IrBinOpCmpGreaterOrEq) {
                bignum_cmp = bignum_cmp_gte;
            } else {
                zig_unreachable();
            }

            answer = bignum_cmp(&op1_val->data.x_bignum, &op2_val->data.x_bignum);
        } else {
            bool are_equal = const_values_equal(op1_val, op2_val, resolved_type);
            if (op_id == IrBinOpCmpEq) {
                answer = are_equal;
            } else if (op_id == IrBinOpCmpNotEq) {
                answer = !are_equal;
            } else {
                zig_unreachable();
            }
        }

        bool depends_on_compile_var = op1_val->depends_on_compile_var || op2_val->depends_on_compile_var;
        ConstExprValue *out_val = ir_build_const_from(ira, &bin_op_instruction->base, depends_on_compile_var);
        out_val->data.x_bool = answer;
        return ira->codegen->builtin_types.entry_bool;
    }

    ir_build_bin_op_from(&ira->new_irb, &bin_op_instruction->base, op_id, casted_op1, casted_op2);

    return ira->codegen->builtin_types.entry_bool;
}

static uint64_t max_unsigned_val(TypeTableEntry *type_entry) {
    assert(type_entry->id == TypeTableEntryIdInt);
    if (type_entry->data.integral.bit_count == 64) {
        return UINT64_MAX;
    } else if (type_entry->data.integral.bit_count == 32) {
        return UINT32_MAX;
    } else if (type_entry->data.integral.bit_count == 16) {
        return UINT16_MAX;
    } else if (type_entry->data.integral.bit_count == 8) {
        return UINT8_MAX;
    } else {
        zig_unreachable();
    }
}

static int ir_eval_bignum(ConstExprValue *op1_val, ConstExprValue *op2_val,
        ConstExprValue *out_val, bool (*bignum_fn)(BigNum *, BigNum *, BigNum *),
        TypeTableEntry *type, bool wrapping_op)
{
    bool overflow = bignum_fn(&out_val->data.x_bignum, &op1_val->data.x_bignum, &op2_val->data.x_bignum);
    if (overflow) {
        return ErrorOverflow;
    }

    if (type->id == TypeTableEntryIdInt && !bignum_fits_in_bits(&out_val->data.x_bignum,
                type->data.integral.bit_count, type->data.integral.is_signed))
    {
        if (wrapping_op) {
            if (type->data.integral.is_signed) {
                out_val->data.x_bignum.data.x_uint = max_unsigned_val(type) - out_val->data.x_bignum.data.x_uint + 1;
                out_val->data.x_bignum.is_negative = !out_val->data.x_bignum.is_negative;
            } else if (out_val->data.x_bignum.is_negative) {
                out_val->data.x_bignum.data.x_uint = max_unsigned_val(type) - out_val->data.x_bignum.data.x_uint + 1;
                out_val->data.x_bignum.is_negative = false;
            } else {
                bignum_truncate(&out_val->data.x_bignum, type->data.integral.bit_count);
            }
        } else {
            return ErrorOverflow;
        }
    }

    out_val->ok = true;
    out_val->depends_on_compile_var = op1_val->depends_on_compile_var || op2_val->depends_on_compile_var;
    return 0;
}

static int ir_eval_math_op(ConstExprValue *op1_val, TypeTableEntry *op1_type,
        IrBinOp op_id, ConstExprValue *op2_val, TypeTableEntry *op2_type, ConstExprValue *out_val)
{
    switch (op_id) {
        case IrBinOpInvalid:
        case IrBinOpBoolOr:
        case IrBinOpBoolAnd:
        case IrBinOpCmpEq:
        case IrBinOpCmpNotEq:
        case IrBinOpCmpLessThan:
        case IrBinOpCmpGreaterThan:
        case IrBinOpCmpLessOrEq:
        case IrBinOpCmpGreaterOrEq:
        case IrBinOpArrayCat:
        case IrBinOpArrayMult:
            zig_unreachable();
        case IrBinOpBinOr:
            return ir_eval_bignum(op1_val, op2_val, out_val, bignum_or, op1_type, false);
        case IrBinOpBinXor:
            return ir_eval_bignum(op1_val, op2_val, out_val, bignum_xor, op1_type, false);
        case IrBinOpBinAnd:
            return ir_eval_bignum(op1_val, op2_val, out_val, bignum_and, op1_type, false);
        case IrBinOpBitShiftLeft:
            return ir_eval_bignum(op1_val, op2_val, out_val, bignum_shl, op1_type, false);
        case IrBinOpBitShiftLeftWrap:
            return ir_eval_bignum(op1_val, op2_val, out_val, bignum_shl, op1_type, true);
        case IrBinOpBitShiftRight:
            return ir_eval_bignum(op1_val, op2_val, out_val, bignum_shr, op1_type, false);
        case IrBinOpAdd:
            return ir_eval_bignum(op1_val, op2_val, out_val, bignum_add, op1_type, false);
        case IrBinOpAddWrap:
            return ir_eval_bignum(op1_val, op2_val, out_val, bignum_add, op1_type, true);
        case IrBinOpSub:
            return ir_eval_bignum(op1_val, op2_val, out_val, bignum_sub, op1_type, false);
        case IrBinOpSubWrap:
            return ir_eval_bignum(op1_val, op2_val, out_val, bignum_sub, op1_type, true);
        case IrBinOpMult:
            return ir_eval_bignum(op1_val, op2_val, out_val, bignum_mul, op1_type, false);
        case IrBinOpMultWrap:
            return ir_eval_bignum(op1_val, op2_val, out_val, bignum_mul, op1_type, true);
        case IrBinOpDiv:
            return ir_eval_bignum(op1_val, op2_val, out_val, bignum_div, op1_type, false);
        case IrBinOpMod:
            return ir_eval_bignum(op1_val, op2_val, out_val, bignum_mod, op1_type, false);
    }
    zig_unreachable();
}

static TypeTableEntry *ir_analyze_bin_op_math(IrAnalyze *ira, IrInstructionBinOp *bin_op_instruction) {
    IrInstruction *op1 = bin_op_instruction->op1->other;
    IrInstruction *op2 = bin_op_instruction->op2->other;
    IrInstruction *instructions[] = {op1, op2};
    TypeTableEntry *resolved_type = ir_resolve_peer_types(ira, bin_op_instruction->base.source_node, instructions, 2);
    if (resolved_type->id == TypeTableEntryIdInvalid)
        return resolved_type;
    IrBinOp op_id = bin_op_instruction->op_id;

    if (resolved_type->id == TypeTableEntryIdInt ||
        resolved_type->id == TypeTableEntryIdNumLitInt)
    {
        // int
    } else if ((resolved_type->id == TypeTableEntryIdFloat ||
                resolved_type->id == TypeTableEntryIdNumLitFloat) &&
        (op_id == IrBinOpAdd ||
            op_id == IrBinOpSub ||
            op_id == IrBinOpMult ||
            op_id == IrBinOpDiv ||
            op_id == IrBinOpMod))
    {
        // float
    } else {
        AstNode *source_node = bin_op_instruction->base.source_node;
        add_node_error(ira->codegen, source_node,
            buf_sprintf("invalid operands to binary expression: '%s' and '%s'",
                buf_ptr(&op1->type_entry->name),
                buf_ptr(&op2->type_entry->name)));
        return ira->codegen->builtin_types.entry_invalid;
    }

    IrInstruction *casted_op1 = ir_get_casted_value(ira, op1, resolved_type);
    if (casted_op1 == ira->codegen->invalid_instruction)
        return ira->codegen->builtin_types.entry_invalid;

    IrInstruction *casted_op2 = ir_get_casted_value(ira, op2, resolved_type);
    if (casted_op2 == ira->codegen->invalid_instruction)
        return ira->codegen->builtin_types.entry_invalid;


    if (casted_op1->static_value.ok && casted_op2->static_value.ok) {
        ConstExprValue *op1_val = &casted_op1->static_value;
        ConstExprValue *op2_val = &casted_op2->static_value;
        ConstExprValue *out_val = &bin_op_instruction->base.static_value;

        bin_op_instruction->base.other = &bin_op_instruction->base;

        int err;
        if ((err = ir_eval_math_op(op1_val, resolved_type, op_id, op2_val, resolved_type, out_val))) {
            if (err == ErrorDivByZero) {
                add_node_error(ira->codegen, bin_op_instruction->base.source_node,
                        buf_sprintf("division by zero is undefined"));
                return ira->codegen->builtin_types.entry_invalid;
            } else if (err == ErrorOverflow) {
                add_node_error(ira->codegen, bin_op_instruction->base.source_node,
                        buf_sprintf("value cannot be represented in any integer type"));
                return ira->codegen->builtin_types.entry_invalid;
            }
            return ira->codegen->builtin_types.entry_invalid;
        }

        ir_num_lit_fits_in_other_type(ira, &bin_op_instruction->base, resolved_type);
        return resolved_type;

    }

    ir_build_bin_op_from(&ira->new_irb, &bin_op_instruction->base, op_id, casted_op1, casted_op2);
    return resolved_type;
}


static TypeTableEntry *ir_analyze_instruction_bin_op(IrAnalyze *ira, IrInstructionBinOp *bin_op_instruction) {
    IrBinOp op_id = bin_op_instruction->op_id;
    switch (op_id) {
        case IrBinOpInvalid:
            zig_unreachable();
        case IrBinOpBoolOr:
        case IrBinOpBoolAnd:
            return ir_analyze_bin_op_bool(ira, bin_op_instruction);
        case IrBinOpCmpEq:
        case IrBinOpCmpNotEq:
        case IrBinOpCmpLessThan:
        case IrBinOpCmpGreaterThan:
        case IrBinOpCmpLessOrEq:
        case IrBinOpCmpGreaterOrEq:
            return ir_analyze_bin_op_cmp(ira, bin_op_instruction);
        case IrBinOpBinOr:
        case IrBinOpBinXor:
        case IrBinOpBinAnd:
        case IrBinOpBitShiftLeft:
        case IrBinOpBitShiftLeftWrap:
        case IrBinOpBitShiftRight:
        case IrBinOpAdd:
        case IrBinOpAddWrap:
        case IrBinOpSub:
        case IrBinOpSubWrap:
        case IrBinOpMult:
        case IrBinOpMultWrap:
        case IrBinOpDiv:
        case IrBinOpMod:
            return ir_analyze_bin_op_math(ira, bin_op_instruction);
        case IrBinOpArrayCat:
        case IrBinOpArrayMult:
            zig_panic("TODO analyze more binary operations");
    }
    zig_unreachable();
}

static TypeTableEntry *ir_analyze_instruction_decl_var(IrAnalyze *ira, IrInstructionDeclVar *decl_var_instruction) {
    IrInstruction *init_value = decl_var_instruction->init_value->other;
    if (init_value->type_entry->id == TypeTableEntryIdInvalid)
        return init_value->type_entry;

    VariableTableEntry *var = decl_var_instruction->var;
    AstNodeVariableDeclaration *variable_declaration = &var->decl_node->data.variable_declaration;
    bool is_export = (variable_declaration->top_level_decl.visib_mod == VisibModExport);
    bool is_extern = variable_declaration->is_extern;

    var->ref_count = 0;

    TypeTableEntry *explicit_type = nullptr;
    IrInstruction *var_type = nullptr;
    if (decl_var_instruction->var_type != nullptr) {
        var_type = decl_var_instruction->var_type->other;
        TypeTableEntry *proposed_type = ir_resolve_type(ira, var_type);
        explicit_type = validate_var_type(ira->codegen, var_type->source_node, proposed_type);
        if (explicit_type->id == TypeTableEntryIdInvalid)
            return explicit_type;
    }

    IrInstruction *casted_init_value = ir_get_casted_value(ira, init_value, explicit_type);
    TypeTableEntry *result_type = get_underlying_type(casted_init_value->type_entry);
    switch (result_type->id) {
        case TypeTableEntryIdTypeDecl:
            zig_unreachable();
        case TypeTableEntryIdInvalid:
            result_type = ira->codegen->builtin_types.entry_invalid;
            break;
        case TypeTableEntryIdNumLitFloat:
        case TypeTableEntryIdNumLitInt:
            if (is_export || is_extern || !casted_init_value->static_value.ok) {
                add_node_error(ira->codegen, var_type->source_node, buf_sprintf("unable to infer variable type"));
                result_type = ira->codegen->builtin_types.entry_invalid;
            }
            break;
        case TypeTableEntryIdUnreachable:
        case TypeTableEntryIdVar:
        case TypeTableEntryIdBlock:
            add_node_error(ira->codegen, var_type->source_node,
                buf_sprintf("variable of type '%s' not allowed", buf_ptr(&result_type->name)));
            result_type = ira->codegen->builtin_types.entry_invalid;
            break;
        case TypeTableEntryIdMetaType:
        case TypeTableEntryIdNamespace:
            if (!casted_init_value->static_value.ok) {
                add_node_error(ira->codegen, var_type->source_node,
                    buf_sprintf("variable of type '%s' must be constant", buf_ptr(&result_type->name)));
                result_type = ira->codegen->builtin_types.entry_invalid;
            }
            break;
        case TypeTableEntryIdUndefLit:
        case TypeTableEntryIdNullLit:
        case TypeTableEntryIdVoid:
        case TypeTableEntryIdBool:
        case TypeTableEntryIdInt:
        case TypeTableEntryIdFloat:
        case TypeTableEntryIdPointer:
        case TypeTableEntryIdArray:
        case TypeTableEntryIdStruct:
        case TypeTableEntryIdMaybe:
        case TypeTableEntryIdErrorUnion:
        case TypeTableEntryIdPureError:
        case TypeTableEntryIdEnum:
        case TypeTableEntryIdUnion:
        case TypeTableEntryIdFn:
        case TypeTableEntryIdGenericFn:
            // OK
            break;
    }

    var->type = result_type;
    assert(var->type);

    ConstExprValue *mem_slot = &ira->exec_context.mem_slot_list[var->mem_slot_index];
    *mem_slot = casted_init_value->static_value;

    ir_build_var_decl_from(&ira->new_irb, &decl_var_instruction->base, var, var_type, casted_init_value);

    BlockContext *scope = decl_var_instruction->base.source_node->block_context;
    if (scope->fn_entry)
        scope->fn_entry->variable_list.append(var);

    return ira->codegen->builtin_types.entry_void;
}

static TypeTableEntry *ir_analyze_instruction_call(IrAnalyze *ira, IrInstructionCall *call_instruction) {
    IrInstruction *fn_ref = call_instruction->fn->other;
    if (fn_ref->type_entry->id == TypeTableEntryIdInvalid)
        return ira->codegen->builtin_types.entry_invalid;

    if (fn_ref->static_value.ok) {
        if (fn_ref->type_entry->id == TypeTableEntryIdMetaType) {
            size_t actual_param_count = call_instruction->arg_count;

            if (actual_param_count != 1) {
                add_node_error(ira->codegen, call_instruction->base.source_node,
                        buf_sprintf("cast expression expects exactly one parameter"));
                return ira->codegen->builtin_types.entry_invalid;
            }

            IrInstruction *arg = call_instruction->args[0];
            IrInstruction *cast_instruction = ir_analyze_cast(ira, &call_instruction->base, fn_ref, arg);
            if (cast_instruction == ira->codegen->invalid_instruction)
                return ira->codegen->builtin_types.entry_invalid;

            ir_link_new_instruction(cast_instruction, &call_instruction->base);
            return cast_instruction->type_entry;
        } else if (fn_ref->type_entry->id == TypeTableEntryIdFn) {
            // TODO fully port over the fn call analyze code to IR
            FnTableEntry *fn_table_entry = fn_ref->static_value.data.x_fn;
            TypeTableEntry *fn_type = fn_table_entry->type_entry;

            IrInstruction **casted_args = allocate<IrInstruction *>(call_instruction->arg_count);
            for (size_t i = 0; i < call_instruction->arg_count; i += 1) {
                TypeTableEntry *param_type = fn_type->data.fn.fn_type_id.param_info[i].type;
                IrInstruction *old_arg = call_instruction->args[i]->other;
                casted_args[i] = ir_get_casted_value(ira, old_arg, param_type);
            }

            ir_build_call_from(&ira->new_irb, &call_instruction->base,
                    call_instruction->fn, call_instruction->arg_count, casted_args);

            TypeTableEntry *return_type = fn_type->data.fn.fn_type_id.return_type;
            return return_type;
        } else {
            zig_panic("TODO analyze more fn call types");
        }
    } else {
        //ir_build_call_from(&ira->new_irb, &call_instruction->base,
        //        call_instruction->fn, call_instruction->arg_count, call_instruction->args);

        zig_panic("TODO analyze fn call");
    }
}

static TypeTableEntry *ir_analyze_unary_bool_not(IrAnalyze *ira, IrInstructionUnOp *un_op_instruction) {
    TypeTableEntry *bool_type = ira->codegen->builtin_types.entry_bool;

    IrInstruction *casted_value = ir_get_casted_value(ira, un_op_instruction->value->other, bool_type);
    if (casted_value == ira->codegen->invalid_instruction)
        return ira->codegen->builtin_types.entry_invalid;

    ConstExprValue *operand_val = &casted_value->static_value;
    if (operand_val->ok) {
        ConstExprValue *result_val = &un_op_instruction->base.static_value;
        result_val->ok = true;
        result_val->depends_on_compile_var = operand_val->depends_on_compile_var;
        result_val->data.x_bool = !operand_val->data.x_bool;
        return bool_type;
    }

    ir_build_un_op_from(&ira->new_irb, &un_op_instruction->base, IrUnOpBoolNot, casted_value);

    return bool_type;
}

static TypeTableEntry *ir_analyze_unary_prefix_op_err(IrAnalyze *ira, IrInstructionUnOp *un_op_instruction) {
    assert(un_op_instruction->op_id == IrUnOpError);
    IrInstruction *value = un_op_instruction->value->other;

    TypeTableEntry *type_entry = value->type_entry;
    if (type_entry->id == TypeTableEntryIdInvalid)
        return ira->codegen->builtin_types.entry_invalid;

    TypeTableEntry *meta_type = ir_resolve_type(ira, value);
    TypeTableEntry *underlying_meta_type = get_underlying_type(meta_type);
    switch (underlying_meta_type->id) {
        case TypeTableEntryIdTypeDecl:
            zig_unreachable();
        case TypeTableEntryIdInvalid:
            return ira->codegen->builtin_types.entry_invalid;
        case TypeTableEntryIdVoid:
        case TypeTableEntryIdBool:
        case TypeTableEntryIdInt:
        case TypeTableEntryIdFloat:
        case TypeTableEntryIdPointer:
        case TypeTableEntryIdArray:
        case TypeTableEntryIdStruct:
        case TypeTableEntryIdMaybe:
        case TypeTableEntryIdErrorUnion:
        case TypeTableEntryIdPureError:
        case TypeTableEntryIdEnum:
        case TypeTableEntryIdUnion:
        case TypeTableEntryIdFn:
        case TypeTableEntryIdGenericFn:
            {
                ConstExprValue *out_val = ir_build_const_from(ira, &un_op_instruction->base,
                        value->static_value.depends_on_compile_var);
                TypeTableEntry *result_type = get_error_type(ira->codegen, meta_type);
                out_val->data.x_type = result_type;
                return ira->codegen->builtin_types.entry_type;
            }
        case TypeTableEntryIdMetaType:
        case TypeTableEntryIdNumLitFloat:
        case TypeTableEntryIdNumLitInt:
        case TypeTableEntryIdUndefLit:
        case TypeTableEntryIdNullLit:
        case TypeTableEntryIdNamespace:
        case TypeTableEntryIdBlock:
        case TypeTableEntryIdUnreachable:
        case TypeTableEntryIdVar:
            add_node_error(ira->codegen, un_op_instruction->base.source_node,
                    buf_sprintf("unable to wrap type '%s' in error type", buf_ptr(&meta_type->name)));
            // TODO if meta_type is type decl, add note pointing to type decl declaration
            return ira->codegen->builtin_types.entry_invalid;
    }
    zig_unreachable();
}

static TypeTableEntry *ir_analyze_instruction_un_op(IrAnalyze *ira, IrInstructionUnOp *un_op_instruction) {
    IrUnOp op_id = un_op_instruction->op_id;
    switch (op_id) {
        case IrUnOpInvalid:
            zig_unreachable();
        case IrUnOpBoolNot:
            return ir_analyze_unary_bool_not(ira, un_op_instruction);
        case IrUnOpBinNot:
            zig_panic("TODO analyze PrefixOpBinNot");
            //{
            //    TypeTableEntry *expr_type = analyze_expression(g, import, context, expected_type,
            //            *expr_node);
            //    if (expr_type->id == TypeTableEntryIdInvalid) {
            //        return expr_type;
            //    } else if (expr_type->id == TypeTableEntryIdInt) {
            //        return expr_type;
            //    } else {
            //        add_node_error(g, node, buf_sprintf("unable to perform binary not operation on type '%s'",
            //                buf_ptr(&expr_type->name)));
            //        return g->builtin_types.entry_invalid;
            //    }
            //    // TODO const expr eval
            //}
        case IrUnOpNegation:
        case IrUnOpNegationWrap:
            zig_panic("TODO analyze PrefixOpNegation[Wrap]");
            //{
            //    TypeTableEntry *expr_type = analyze_expression(g, import, context, nullptr, *expr_node);
            //    if (expr_type->id == TypeTableEntryIdInvalid) {
            //        return expr_type;
            //    } else if ((expr_type->id == TypeTableEntryIdInt &&
            //                expr_type->data.integral.is_signed) ||
            //                expr_type->id == TypeTableEntryIdNumLitInt ||
            //                ((expr_type->id == TypeTableEntryIdFloat ||
            //                expr_type->id == TypeTableEntryIdNumLitFloat) &&
            //                prefix_op != PrefixOpNegationWrap))
            //    {
            //        ConstExprValue *target_const_val = &get_resolved_expr(*expr_node)->const_val;
            //        if (!target_const_val->ok) {
            //            return expr_type;
            //        }
            //        ConstExprValue *const_val = &get_resolved_expr(node)->const_val;
            //        const_val->ok = true;
            //        const_val->depends_on_compile_var = target_const_val->depends_on_compile_var;
            //        bignum_negate(&const_val->data.x_bignum, &target_const_val->data.x_bignum);
            //        if (expr_type->id == TypeTableEntryIdFloat ||
            //            expr_type->id == TypeTableEntryIdNumLitFloat ||
            //            expr_type->id == TypeTableEntryIdNumLitInt)
            //        {
            //            return expr_type;
            //        }

            //        bool overflow = !bignum_fits_in_bits(&const_val->data.x_bignum,
            //                expr_type->data.integral.bit_count, expr_type->data.integral.is_signed);
            //        if (prefix_op == PrefixOpNegationWrap) {
            //            if (overflow) {
            //                const_val->data.x_bignum.is_negative = true;
            //            }
            //        } else if (overflow) {
            //            add_node_error(g, *expr_node, buf_sprintf("negation caused overflow"));
            //            return g->builtin_types.entry_invalid;
            //        }
            //        return expr_type;
            //    } else {
            //        const char *fmt = (prefix_op == PrefixOpNegationWrap) ?
            //            "invalid wrapping negation type: '%s'" : "invalid negation type: '%s'";
            //        add_node_error(g, node, buf_sprintf(fmt, buf_ptr(&expr_type->name)));
            //        return g->builtin_types.entry_invalid;
            //    }
            //}
        case IrUnOpAddressOf:
        case IrUnOpConstAddressOf:
            zig_panic("TODO analyze PrefixOpAddressOf and PrefixOpConstAddressOf");
            //{
            //    bool is_const = (prefix_op == PrefixOpConstAddressOf);

            //    TypeTableEntry *child_type = analyze_lvalue(g, import, context,
            //            *expr_node, LValPurposeAddressOf, is_const);

            //    if (child_type->id == TypeTableEntryIdInvalid) {
            //        return g->builtin_types.entry_invalid;
            //    } else if (child_type->id == TypeTableEntryIdMetaType) {
            //        TypeTableEntry *meta_type = analyze_type_expr_pointer_only(g, import, context,
            //                *expr_node, true);
            //        if (meta_type->id == TypeTableEntryIdInvalid) {
            //            return g->builtin_types.entry_invalid;
            //        } else if (meta_type->id == TypeTableEntryIdUnreachable) {
            //            add_node_error(g, node, buf_create_from_str("pointer to unreachable not allowed"));
            //            return g->builtin_types.entry_invalid;
            //        } else {
            //            return resolve_expr_const_val_as_type(g, node,
            //                    get_pointer_to_type(g, meta_type, is_const), false);
            //        }
            //    } else if (child_type->id == TypeTableEntryIdNumLitInt ||
            //               child_type->id == TypeTableEntryIdNumLitFloat)
            //    {
            //        add_node_error(g, *expr_node,
            //            buf_sprintf("unable to get address of type '%s'", buf_ptr(&child_type->name)));
            //        return g->builtin_types.entry_invalid;
            //    } else {
            //        return get_pointer_to_type(g, child_type, is_const);
            //    }
            //}
        case IrUnOpDereference:
            zig_panic("TODO remove this IrUnOp item");
        case IrUnOpMaybe:
            zig_panic("TODO analyze PrefixOpMaybe");
            //{
            //    TypeTableEntry *type_entry = analyze_expression(g, import, context, nullptr, *expr_node);

            //    if (type_entry->id == TypeTableEntryIdInvalid) {
            //        return type_entry;
            //    } else if (type_entry->id == TypeTableEntryIdMetaType) {
            //        TypeTableEntry *meta_type = resolve_type(g, *expr_node);
            //        if (meta_type->id == TypeTableEntryIdInvalid) {
            //            return g->builtin_types.entry_invalid;
            //        } else if (meta_type->id == TypeTableEntryIdUnreachable) {
            //            add_node_error(g, node, buf_create_from_str("unable to wrap unreachable in maybe type"));
            //            return g->builtin_types.entry_invalid;
            //        } else {
            //            return resolve_expr_const_val_as_type(g, node, get_maybe_type(g, meta_type), false);
            //        }
            //    } else if (type_entry->id == TypeTableEntryIdUnreachable) {
            //        add_node_error(g, *expr_node, buf_sprintf("unable to wrap unreachable in maybe type"));
            //        return g->builtin_types.entry_invalid;
            //    } else {
            //        ConstExprValue *target_const_val = &get_resolved_expr(*expr_node)->const_val;
            //        TypeTableEntry *maybe_type = get_maybe_type(g, type_entry);
            //        if (!target_const_val->ok) {
            //            return maybe_type;
            //        }
            //        return resolve_expr_const_val_as_non_null(g, node, maybe_type, target_const_val);
            //    }
            //}
        case IrUnOpError:
            return ir_analyze_unary_prefix_op_err(ira, un_op_instruction);
        case IrUnOpUnwrapError:
            zig_panic("TODO analyze PrefixOpUnwrapError");
            //{
            //    TypeTableEntry *type_entry = analyze_expression(g, import, context, nullptr, *expr_node);

            //    if (type_entry->id == TypeTableEntryIdInvalid) {
            //        return type_entry;
            //    } else if (type_entry->id == TypeTableEntryIdErrorUnion) {
            //        return type_entry->data.error.child_type;
            //    } else {
            //        add_node_error(g, *expr_node,
            //            buf_sprintf("expected error type, got '%s'", buf_ptr(&type_entry->name)));
            //        return g->builtin_types.entry_invalid;
            //    }
            //}
        case IrUnOpUnwrapMaybe:
            zig_panic("TODO analyze PrefixOpUnwrapMaybe");
            //{
            //    TypeTableEntry *type_entry = analyze_expression(g, import, context, nullptr, *expr_node);

            //    if (type_entry->id == TypeTableEntryIdInvalid) {
            //        return type_entry;
            //    } else if (type_entry->id == TypeTableEntryIdMaybe) {
            //        return type_entry->data.maybe.child_type;
            //    } else {
            //        add_node_error(g, *expr_node,
            //            buf_sprintf("expected maybe type, got '%s'", buf_ptr(&type_entry->name)));
            //        return g->builtin_types.entry_invalid;
            //    }
            //}
        case IrUnOpErrorReturn:
            zig_panic("TODO analyze IrUnOpErrorReturn");
        case IrUnOpMaybeReturn:
            zig_panic("TODO analyze IrUnOpMaybeReturn");
    }
    zig_unreachable();
}

static TypeTableEntry *ir_analyze_instruction_br(IrAnalyze *ira, IrInstructionBr *br_instruction) {
    IrBasicBlock *old_dest_block = br_instruction->dest_block;

    // TODO detect backward jumps

    if (br_instruction->is_inline || old_dest_block->ref_count == 1) {
        ir_inline_bb(ira, old_dest_block);
        return ira->codegen->builtin_types.entry_unreachable;
    }

    IrBasicBlock *new_bb = ir_get_new_bb(ira, old_dest_block);
    ir_build_br_from(&ira->new_irb, &br_instruction->base, new_bb);
    ir_finish_bb(ira);
    return ira->codegen->builtin_types.entry_unreachable;
}

static TypeTableEntry *ir_analyze_instruction_cond_br(IrAnalyze *ira, IrInstructionCondBr *cond_br_instruction) {
    TypeTableEntry *bool_type = ira->codegen->builtin_types.entry_bool;
    IrInstruction *condition = ir_get_casted_value(ira, cond_br_instruction->condition->other, bool_type);
    if (condition == ira->codegen->invalid_instruction) {
        ir_finish_bb(ira);
        return ira->codegen->builtin_types.entry_unreachable;
    }

    // TODO detect backward jumps
    if (condition->static_value.ok) {
        IrBasicBlock *old_dest_block = condition->static_value.data.x_bool ?
            cond_br_instruction->then_block : cond_br_instruction->else_block;

        if (cond_br_instruction->is_inline || old_dest_block->ref_count == 1) {
            ir_inline_bb(ira, old_dest_block);
            return ira->codegen->builtin_types.entry_unreachable;
        }
    } else if (cond_br_instruction->is_inline) {
        add_node_error(ira->codegen, condition->source_node,
                buf_sprintf("unable to evaluate constant expression"));
        ir_finish_bb(ira);
        return ira->codegen->builtin_types.entry_unreachable;
    }

    IrBasicBlock *new_then_block = ir_get_new_bb(ira, cond_br_instruction->then_block);
    IrBasicBlock *new_else_block = ir_get_new_bb(ira, cond_br_instruction->else_block);
    ir_build_cond_br_from(&ira->new_irb, &cond_br_instruction->base, condition, new_then_block, new_else_block, false);
    ir_finish_bb(ira);
    return ira->codegen->builtin_types.entry_unreachable;
}

static TypeTableEntry *ir_analyze_instruction_unreachable(IrAnalyze *ira,
        IrInstructionUnreachable *unreachable_instruction)
{
    ir_build_unreachable_from(&ira->new_irb, &unreachable_instruction->base);
    ir_finish_bb(ira);
    return ira->codegen->builtin_types.entry_unreachable;
}

static TypeTableEntry *ir_analyze_instruction_phi(IrAnalyze *ira, IrInstructionPhi *phi_instruction) {
    if (ira->const_predecessor_bb) {
        for (size_t i = 0; i < phi_instruction->incoming_count; i += 1) {
            IrBasicBlock *predecessor = phi_instruction->incoming_blocks[i];
            if (predecessor != ira->const_predecessor_bb)
                continue;
            IrInstruction *value = phi_instruction->incoming_values[i]->other;
            assert(value->type_entry);
            if (value->static_value.ok) {
                ConstExprValue *out_val = ir_build_const_from(ira, &phi_instruction->base,
                        value->static_value.depends_on_compile_var);
                *out_val = value->static_value;
            } else {
                phi_instruction->base.other = value;
            }
            return value->type_entry;
        }
        zig_unreachable();
    }

    ZigList<IrBasicBlock*> new_incoming_blocks = {0};
    ZigList<IrInstruction*> new_incoming_values = {0};

    for (size_t i = 0; i < phi_instruction->incoming_count; i += 1) {
        IrBasicBlock *predecessor = phi_instruction->incoming_blocks[i];
        if (predecessor->ref_count == 0)
            continue;

        assert(predecessor->other);
        new_incoming_blocks.append(predecessor->other);

        IrInstruction *old_value = phi_instruction->incoming_values[i];
        assert(old_value);
        new_incoming_values.append(old_value->other);
    }
    assert(new_incoming_blocks.length != 0);

    if (new_incoming_blocks.length == 1) {
        IrInstruction *first_value = new_incoming_values.at(0);
        phi_instruction->base.other = first_value;
        return first_value->type_entry;
    }

    TypeTableEntry *resolved_type = ir_resolve_peer_types(ira, phi_instruction->base.source_node,
            new_incoming_values.items, new_incoming_values.length);
    if (resolved_type->id == TypeTableEntryIdInvalid)
        return resolved_type;

    ir_build_phi_from(&ira->new_irb, &phi_instruction->base, new_incoming_blocks.length,
            new_incoming_blocks.items, new_incoming_values.items);
    return resolved_type;
}

static TypeTableEntry *ir_analyze_instruction_var_ptr(IrAnalyze *ira, IrInstructionVarPtr *var_ptr_instruction) {
    VariableTableEntry *var = var_ptr_instruction->var;
    if (var->type->id == TypeTableEntryIdInvalid)
        return var->type;

    TypeTableEntry *ptr_type = get_pointer_to_type(ira->codegen, var->type, false);
    // TODO once the anlayze code is fully ported over to IR we won't need this SIZE_MAX thing.
    if (var->mem_slot_index != SIZE_MAX) {
        ConstExprValue *mem_slot = &ira->exec_context.mem_slot_list[var->mem_slot_index];
        if (mem_slot->ok) {
            ConstExprValue *out_val = ir_build_const_from(ira, &var_ptr_instruction->base,
                    mem_slot->depends_on_compile_var);

            out_val->data.x_ptr.len = 1;
            out_val->data.x_ptr.is_c_str = false;
            out_val->data.x_ptr.ptr = allocate<ConstExprValue *>(1);
            out_val->data.x_ptr.ptr[0] = mem_slot;
            return ptr_type;
        }
    }

    ir_build_var_ptr_from(&ira->new_irb, &var_ptr_instruction->base, var);
    return ptr_type;
}

static TypeTableEntry *ir_analyze_instruction_elem_ptr(IrAnalyze *ira, IrInstructionElemPtr *elem_ptr_instruction) {
    IrInstruction *array_ptr = elem_ptr_instruction->array_ptr->other;
    IrInstruction *elem_index = elem_ptr_instruction->elem_index->other;

    TypeTableEntry *array_type = array_ptr->type_entry;
    TypeTableEntry *return_type;

    if (array_type->id == TypeTableEntryIdInvalid) {
        return array_type;
    } else if (array_type->id == TypeTableEntryIdArray) {
        if (array_type->data.array.len == 0) {
            add_node_error(ira->codegen, elem_ptr_instruction->base.source_node,
                    buf_sprintf("out of bounds array access"));
        }
        TypeTableEntry *child_type = array_type->data.array.child_type;
        return_type = get_pointer_to_type(ira->codegen, child_type, false);
    } else if (array_type->id == TypeTableEntryIdPointer) {
        return_type = array_type;
    } else if (is_slice(array_type)) {
        return_type = array_type->data.structure.fields[0].type_entry;
    } else {
        add_node_error(ira->codegen, elem_ptr_instruction->base.source_node,
                buf_sprintf("array access of non-array type '%s'", buf_ptr(&array_type->name)));
        return ira->codegen->builtin_types.entry_invalid;
    }

    TypeTableEntry *usize = ira->codegen->builtin_types.entry_usize;
    IrInstruction *casted_elem_index = ir_get_casted_value(ira, elem_index, usize);
    if (casted_elem_index == ira->codegen->invalid_instruction)
        return ira->codegen->builtin_types.entry_invalid;

    if (array_ptr->static_value.ok && casted_elem_index->static_value.ok) {
        zig_panic("TODO compile time array access");
    }

    ir_build_elem_ptr_from(&ira->new_irb, &elem_ptr_instruction->base, array_ptr, casted_elem_index);

    return return_type;
}

static TypeTableEntry *ir_analyze_container_member_access_inner(IrAnalyze *ira,
    TypeTableEntry *bare_struct_type, Buf *field_name, IrInstructionFieldPtr *field_ptr_instruction,
    TypeTableEntry *container_type)
{
    if (!is_slice(bare_struct_type)) {
        BlockContext *container_block_context = get_container_block_context(bare_struct_type);
        assert(container_block_context);
        auto entry = container_block_context->decl_table.maybe_get(field_name);
        AstNode *fn_decl_node = entry ? entry->value : nullptr;
        if (fn_decl_node && fn_decl_node->type == NodeTypeFnProto) {
            zig_panic("TODO member function call");
        }
    }
    add_node_error(ira->codegen, field_ptr_instruction->base.source_node,
        buf_sprintf("no member named '%s' in '%s'", buf_ptr(field_name), buf_ptr(&bare_struct_type->name)));
    return ira->codegen->builtin_types.entry_invalid;
}


static TypeTableEntry *ir_analyze_container_member_access(IrAnalyze *ira, Buf *field_name,
    IrInstructionFieldPtr *field_ptr_instruction, TypeTableEntry *container_type)
{
    IrInstruction *container_ptr = field_ptr_instruction->container_ptr->other;
    TypeTableEntry *bare_type = container_ref_type(container_type);
    if (!type_is_complete(bare_type)) {
        resolve_container_type(ira->codegen, bare_type);
    }

    if (bare_type->id == TypeTableEntryIdStruct) {
        TypeStructField *field = find_struct_type_field(bare_type, field_name);
        if (field) {
            ir_build_struct_field_ptr_from(&ira->new_irb, &field_ptr_instruction->base, container_ptr, field);
            return get_pointer_to_type(ira->codegen, field->type_entry, false);
        } else {
            return ir_analyze_container_member_access_inner(ira, bare_type, field_name,
                field_ptr_instruction, container_type);
        }
    } else if (bare_type->id == TypeTableEntryIdEnum) {
        zig_panic("TODO enum field ptr");
    } else if (bare_type->id == TypeTableEntryIdUnion) {
        zig_panic("TODO");
    } else {
        zig_unreachable();
    }
}

static TypeTableEntry *ir_analyze_instruction_field_ptr(IrAnalyze *ira, IrInstructionFieldPtr *field_ptr_instruction) {
    IrInstruction *container_ptr = field_ptr_instruction->container_ptr->other;
    Buf *field_name = field_ptr_instruction->field_name;

    TypeTableEntry *container_type = container_ptr->type_entry;
    if (container_type->id == TypeTableEntryIdInvalid) {
        return container_type;
    } else if (is_container_ref(container_type)) {
        return ir_analyze_container_member_access(ira, field_name, field_ptr_instruction, container_type);
    } else if (container_type->id == TypeTableEntryIdArray) {
        if (buf_eql_str(field_name, "len")) {
            add_node_error(ira->codegen, field_ptr_instruction->base.source_node,
                buf_sprintf("pointer to array length not available"));
            return ira->codegen->builtin_types.entry_invalid;
        } else {
            add_node_error(ira->codegen, field_ptr_instruction->base.source_node,
                buf_sprintf("no member named '%s' in '%s'", buf_ptr(field_name),
                    buf_ptr(&container_type->name)));
            return ira->codegen->builtin_types.entry_invalid;
        }
    } else if (container_type->id == TypeTableEntryIdMetaType) {
        TypeTableEntry *child_type = ir_resolve_type(ira, container_ptr);

        if (child_type->id == TypeTableEntryIdInvalid) {
            return ira->codegen->builtin_types.entry_invalid;
        } else if (child_type->id == TypeTableEntryIdEnum) {
            zig_panic("TODO enum type field");
        } else if (child_type->id == TypeTableEntryIdStruct) {
            zig_panic("TODO struct type field");
        } else if (child_type->id == TypeTableEntryIdPureError) {
            zig_panic("TODO error type field");
        } else if (child_type->id == TypeTableEntryIdInt) {
            zig_panic("TODO integer type field");
        } else {
            add_node_error(ira->codegen, field_ptr_instruction->base.source_node,
                buf_sprintf("type '%s' does not support field access", buf_ptr(&container_type->name)));
            return ira->codegen->builtin_types.entry_invalid;
        }
    } else if (container_type->id == TypeTableEntryIdNamespace) {
        zig_panic("TODO namespace field access");
    } else {
        add_node_error(ira->codegen, field_ptr_instruction->base.source_node,
            buf_sprintf("type '%s' does not support field access", buf_ptr(&container_type->name)));
        return ira->codegen->builtin_types.entry_invalid;
    }
}

static TypeTableEntry *ir_analyze_read_field_as_ptr_load(IrAnalyze *ira,
    IrInstructionReadField *read_field_instruction)
{
    IrInstruction *old_field_ptr_inst = ir_build_field_ptr(&ira->old_irb, read_field_instruction->base.source_node,
        read_field_instruction->container_ptr, read_field_instruction->field_name);
    IrInstruction *old_load_ptr_inst = ir_build_load_ptr(&ira->old_irb, read_field_instruction->base.source_node,
        old_field_ptr_inst);
    ir_analyze_instruction(ira, old_field_ptr_inst);
    TypeTableEntry *result_type = ir_analyze_instruction(ira, old_load_ptr_inst);
    read_field_instruction->base.other = old_load_ptr_inst->other;
    return result_type;
}

static TypeTableEntry *ir_analyze_instruction_read_field(IrAnalyze *ira,
    IrInstructionReadField *read_field_instruction)
{
    IrInstruction *container_ptr = read_field_instruction->container_ptr->other;
    Buf *field_name = read_field_instruction->field_name;

    TypeTableEntry *container_type = container_ptr->type_entry;
    if (container_type->id == TypeTableEntryIdInvalid) {
        return container_type;
    } else if (is_container_ref(container_type)) {
        return ir_analyze_read_field_as_ptr_load(ira, read_field_instruction);
    } else if (container_type->id == TypeTableEntryIdArray) {
        if (buf_eql_str(field_name, "len")) {
            return ir_analyze_const_usize(ira, &read_field_instruction->base, container_type->data.array.len, false);
        } else {
            add_node_error(ira->codegen, read_field_instruction->base.source_node,
                buf_sprintf("no member named '%s' in '%s'", buf_ptr(field_name),
                    buf_ptr(&container_type->name)));
            return ira->codegen->builtin_types.entry_invalid;
        }
    } else if (container_type->id == TypeTableEntryIdMetaType) {
        TypeTableEntry *child_type = ir_resolve_type(ira, container_ptr);

        if (child_type->id == TypeTableEntryIdInvalid) {
            return ira->codegen->builtin_types.entry_invalid;
        } else if (child_type->id == TypeTableEntryIdEnum) {
            zig_panic("TODO enum type field");
        } else if (child_type->id == TypeTableEntryIdStruct) {
            zig_panic("TODO struct type field");
        } else if (child_type->id == TypeTableEntryIdPureError) {
            zig_panic("TODO error type field");
        } else if (child_type->id == TypeTableEntryIdInt) {
            zig_panic("TODO integer type field");
        } else {
            add_node_error(ira->codegen, read_field_instruction->base.source_node,
                buf_sprintf("type '%s' does not support field access", buf_ptr(&container_type->name)));
            return ira->codegen->builtin_types.entry_invalid;
        }
    } else if (container_type->id == TypeTableEntryIdNamespace) {
        zig_panic("TODO namespace field access");
    } else {
        add_node_error(ira->codegen, read_field_instruction->base.source_node,
            buf_sprintf("type '%s' does not support field access", buf_ptr(&container_type->name)));
        return ira->codegen->builtin_types.entry_invalid;
    }
}

static TypeTableEntry *ir_analyze_instruction_load_ptr(IrAnalyze *ira, IrInstructionLoadPtr *load_ptr_instruction) {
    IrInstruction *ptr = load_ptr_instruction->ptr->other;
    TypeTableEntry *type_entry = ptr->type_entry;
    if (type_entry->id == TypeTableEntryIdInvalid) {
        return type_entry;
    } else if (type_entry->id == TypeTableEntryIdPointer) {
        TypeTableEntry *child_type = type_entry->data.pointer.child_type;
        if (ptr->static_value.ok) {
            ConstExprValue *pointee = ptr->static_value.data.x_ptr.ptr[0];
            if (pointee->ok) {
                ConstExprValue *out_val = ir_build_const_from(ira, &load_ptr_instruction->base,
                        pointee->depends_on_compile_var);
                *out_val = *pointee;
                return child_type;
            }
        }
        ir_build_load_ptr_from(&ira->new_irb, &load_ptr_instruction->base, ptr);
        return child_type;
    } else {
        add_node_error(ira->codegen, load_ptr_instruction->base.source_node,
            buf_sprintf("indirection requires pointer operand ('%s' invalid)",
                buf_ptr(&type_entry->name)));
        return ira->codegen->builtin_types.entry_invalid;
    }
}

static TypeTableEntry *ir_analyze_instruction_store_ptr(IrAnalyze *ira, IrInstructionStorePtr *store_ptr_instruction) {
    IrInstruction *ptr = store_ptr_instruction->ptr->other;
    if (ptr->type_entry->id == TypeTableEntryIdInvalid)
        return ptr->type_entry;

    IrInstruction *value = store_ptr_instruction->value->other;
    if (value->type_entry->id == TypeTableEntryIdInvalid)
        return value->type_entry;

    TypeTableEntry *child_type = ptr->type_entry->data.pointer.child_type;
    IrInstruction *casted_value = ir_get_casted_value(ira, value, child_type);
    if (casted_value == ira->codegen->invalid_instruction)
        return ira->codegen->builtin_types.entry_invalid;

    if (ptr->static_value.ok && casted_value->static_value.ok) {
        ConstExprValue *dest_val = ptr->static_value.data.x_ptr.ptr[0];
        if (dest_val->ok) {
            *dest_val = casted_value->static_value;
            return ir_analyze_void(ira, &store_ptr_instruction->base);
        }
    }

    if (ptr->static_value.ok) {
        // This memory location is transforming from known at compile time to known at runtime.
        // We must emit our own var ptr instruction.
        ptr->static_value.ok = false;
        IrInstruction *new_ptr_inst;
        if (ptr->id == IrInstructionIdVarPtr) {
            IrInstructionVarPtr *var_ptr_inst = (IrInstructionVarPtr *)ptr;
            VariableTableEntry *var = var_ptr_inst->var;
            new_ptr_inst = ir_build_var_ptr(&ira->new_irb, store_ptr_instruction->base.source_node, var);
            assert(var->mem_slot_index != SIZE_MAX);
            ConstExprValue *mem_slot = &ira->exec_context.mem_slot_list[var->mem_slot_index];
            mem_slot->ok = false;
        } else if (ptr->id == IrInstructionIdFieldPtr) {
            zig_panic("TODO");
        } else if (ptr->id == IrInstructionIdElemPtr) {
            zig_panic("TODO");
        } else {
            zig_unreachable();
        }
        new_ptr_inst->type_entry = ptr->type_entry;
        ir_build_store_ptr(&ira->new_irb, store_ptr_instruction->base.source_node, new_ptr_inst, casted_value);
        return ir_analyze_void(ira, &store_ptr_instruction->base);
    }

    ir_build_store_ptr_from(&ira->new_irb, &store_ptr_instruction->base, ptr, casted_value);
    return ira->codegen->builtin_types.entry_void;
}

static TypeTableEntry *ir_analyze_instruction_typeof(IrAnalyze *ira, IrInstructionTypeOf *typeof_instruction) {
    IrInstruction *expr_value = typeof_instruction->value->other;
    TypeTableEntry *type_entry = expr_value->type_entry;
    switch (type_entry->id) {
        case TypeTableEntryIdInvalid:
            return type_entry;
        case TypeTableEntryIdVar:
            add_node_error(ira->codegen, expr_value->source_node,
                    buf_sprintf("type '%s' not eligible for @typeOf", buf_ptr(&type_entry->name)));
            return ira->codegen->builtin_types.entry_invalid;
        case TypeTableEntryIdNumLitFloat:
        case TypeTableEntryIdNumLitInt:
        case TypeTableEntryIdUndefLit:
        case TypeTableEntryIdNullLit:
        case TypeTableEntryIdNamespace:
        case TypeTableEntryIdBlock:
        case TypeTableEntryIdGenericFn:
        case TypeTableEntryIdMetaType:
        case TypeTableEntryIdVoid:
        case TypeTableEntryIdBool:
        case TypeTableEntryIdUnreachable:
        case TypeTableEntryIdInt:
        case TypeTableEntryIdFloat:
        case TypeTableEntryIdPointer:
        case TypeTableEntryIdArray:
        case TypeTableEntryIdStruct:
        case TypeTableEntryIdMaybe:
        case TypeTableEntryIdErrorUnion:
        case TypeTableEntryIdPureError:
        case TypeTableEntryIdEnum:
        case TypeTableEntryIdUnion:
        case TypeTableEntryIdFn:
        case TypeTableEntryIdTypeDecl:
            {
                ConstExprValue *out_val = ir_build_const_from(ira, &typeof_instruction->base, false);
                // TODO depends_on_compile_var should be set based on whether the type of the expression 
                // depends_on_compile_var. but we currently don't have a thing to tell us if the type of
                // something depends on a compile var
                out_val->data.x_type = type_entry;

                return ira->codegen->builtin_types.entry_type;
            }
    }

    zig_unreachable();
}

static TypeTableEntry *ir_analyze_instruction_to_ptr_type(IrAnalyze *ira,
        IrInstructionToPtrType *to_ptr_type_instruction)
{
    IrInstruction *type_value = to_ptr_type_instruction->value->other;
    TypeTableEntry *type_entry = ir_resolve_type(ira, type_value);
    if (type_entry->id == TypeTableEntryIdInvalid)
        return type_entry;

    TypeTableEntry *ptr_type;
    if (type_entry->id == TypeTableEntryIdArray) {
        ptr_type = get_pointer_to_type(ira->codegen, type_entry->data.array.child_type, false);
    } else if (is_slice(type_entry)) {
        ptr_type = type_entry->data.structure.fields[0].type_entry;
    } else {
        add_node_error(ira->codegen, to_ptr_type_instruction->base.source_node,
                buf_sprintf("expected array type, found '%s'", buf_ptr(&type_entry->name)));
        return ira->codegen->builtin_types.entry_invalid;
    }

    ConstExprValue *out_val = ir_build_const_from(ira, &to_ptr_type_instruction->base,
            type_value->static_value.depends_on_compile_var);
    out_val->data.x_type = ptr_type;
    return ira->codegen->builtin_types.entry_type;
}

static TypeTableEntry *ir_analyze_instruction_ptr_type_child(IrAnalyze *ira,
        IrInstructionPtrTypeChild *ptr_type_child_instruction)
{
    IrInstruction *type_value = ptr_type_child_instruction->value->other;
    TypeTableEntry *type_entry = ir_resolve_type(ira, type_value);
    if (type_entry->id == TypeTableEntryIdInvalid)
        return type_entry;

    if (type_entry->id != TypeTableEntryIdPointer) {
        add_node_error(ira->codegen, ptr_type_child_instruction->base.source_node,
                buf_sprintf("expected pointer type, found '%s'", buf_ptr(&type_entry->name)));
        return ira->codegen->builtin_types.entry_invalid;
    }

    ConstExprValue *out_val = ir_build_const_from(ira, &ptr_type_child_instruction->base,
            type_value->static_value.depends_on_compile_var);
    out_val->data.x_type = type_entry->data.pointer.child_type;
    return ira->codegen->builtin_types.entry_type;
}

static TypeTableEntry *ir_analyze_instruction_set_fn_test(IrAnalyze *ira,
        IrInstructionSetFnTest *set_fn_test_instruction)
{
    IrInstruction *fn_value = set_fn_test_instruction->fn_value->other;
    IrInstruction *is_test_value = set_fn_test_instruction->is_test->other;

    FnTableEntry *fn_entry = ir_resolve_fn(ira, fn_value);
    if (!fn_entry)
        return ira->codegen->builtin_types.entry_invalid;

    if (!ir_resolve_bool(ira, is_test_value, &fn_entry->is_test))
        return ira->codegen->builtin_types.entry_invalid;

    AstNode *source_node = set_fn_test_instruction->base.source_node;
    if (fn_entry->fn_test_set_node) {
        ErrorMsg *msg = add_node_error(ira->codegen, source_node,
                buf_sprintf("function test attribute set twice"));
        add_error_note(ira->codegen, msg, fn_entry->fn_test_set_node, buf_sprintf("first set here"));
        return ira->codegen->builtin_types.entry_invalid;
    }
    fn_entry->fn_test_set_node = source_node;

    ira->codegen->test_fn_count += 1;

    ir_build_const_from(ira, &set_fn_test_instruction->base, false);
    return ira->codegen->builtin_types.entry_void;
}

static TypeTableEntry *ir_analyze_instruction_slice_type(IrAnalyze *ira,
        IrInstructionSliceType *slice_type_instruction)
{
    IrInstruction *child_type = slice_type_instruction->child_type->other;
    if (child_type->type_entry->id == TypeTableEntryIdInvalid)
        return ira->codegen->builtin_types.entry_invalid;
    bool is_const = slice_type_instruction->is_const;

    TypeTableEntry *resolved_child_type = ir_resolve_type(ira, child_type);
    TypeTableEntry *canon_child_type = get_underlying_type(resolved_child_type);
    switch (canon_child_type->id) {
        case TypeTableEntryIdTypeDecl:
            zig_unreachable();
        case TypeTableEntryIdInvalid:
            return ira->codegen->builtin_types.entry_invalid;
        case TypeTableEntryIdVar:
        case TypeTableEntryIdUnreachable:
        case TypeTableEntryIdUndefLit:
        case TypeTableEntryIdNullLit:
        case TypeTableEntryIdBlock:
            add_node_error(ira->codegen, slice_type_instruction->base.source_node,
                    buf_sprintf("slice of type '%s' not allowed", buf_ptr(&resolved_child_type->name)));
            // TODO if this is a typedecl, add error note showing the declaration of the type decl
            return ira->codegen->builtin_types.entry_invalid;
        case TypeTableEntryIdMetaType:
        case TypeTableEntryIdVoid:
        case TypeTableEntryIdBool:
        case TypeTableEntryIdInt:
        case TypeTableEntryIdFloat:
        case TypeTableEntryIdPointer:
        case TypeTableEntryIdArray:
        case TypeTableEntryIdStruct:
        case TypeTableEntryIdNumLitFloat:
        case TypeTableEntryIdNumLitInt:
        case TypeTableEntryIdMaybe:
        case TypeTableEntryIdErrorUnion:
        case TypeTableEntryIdPureError:
        case TypeTableEntryIdEnum:
        case TypeTableEntryIdUnion:
        case TypeTableEntryIdFn:
        case TypeTableEntryIdNamespace:
        case TypeTableEntryIdGenericFn:
            {
                TypeTableEntry *result_type = get_slice_type(ira->codegen, resolved_child_type, is_const);
                ConstExprValue *out_val = ir_build_const_from(ira, &slice_type_instruction->base,
                        child_type->static_value.depends_on_compile_var);
                out_val->data.x_type = result_type;
                return ira->codegen->builtin_types.entry_type;
            }
    }
    zig_unreachable();
}

static TypeTableEntry *ir_analyze_instruction_nocast(IrAnalyze *ira, IrInstruction *instruction) {
    switch (instruction->id) {
        case IrInstructionIdInvalid:
            zig_unreachable();
        case IrInstructionIdReturn:
            return ir_analyze_instruction_return(ira, (IrInstructionReturn *)instruction);
        case IrInstructionIdConst:
            return ir_analyze_instruction_const(ira, (IrInstructionConst *)instruction);
        case IrInstructionIdUnOp:
            return ir_analyze_instruction_un_op(ira, (IrInstructionUnOp *)instruction);
        case IrInstructionIdBinOp:
            return ir_analyze_instruction_bin_op(ira, (IrInstructionBinOp *)instruction);
        case IrInstructionIdDeclVar:
            return ir_analyze_instruction_decl_var(ira, (IrInstructionDeclVar *)instruction);
        case IrInstructionIdLoadPtr:
            return ir_analyze_instruction_load_ptr(ira, (IrInstructionLoadPtr *)instruction);
        case IrInstructionIdStorePtr:
            return ir_analyze_instruction_store_ptr(ira, (IrInstructionStorePtr *)instruction);
        case IrInstructionIdElemPtr:
            return ir_analyze_instruction_elem_ptr(ira, (IrInstructionElemPtr *)instruction);
        case IrInstructionIdVarPtr:
            return ir_analyze_instruction_var_ptr(ira, (IrInstructionVarPtr *)instruction);
        case IrInstructionIdFieldPtr:
            return ir_analyze_instruction_field_ptr(ira, (IrInstructionFieldPtr *)instruction);
        case IrInstructionIdReadField:
            return ir_analyze_instruction_read_field(ira, (IrInstructionReadField *)instruction);
        case IrInstructionIdCall:
            return ir_analyze_instruction_call(ira, (IrInstructionCall *)instruction);
        case IrInstructionIdBr:
            return ir_analyze_instruction_br(ira, (IrInstructionBr *)instruction);
        case IrInstructionIdCondBr:
            return ir_analyze_instruction_cond_br(ira, (IrInstructionCondBr *)instruction);
        case IrInstructionIdUnreachable:
            return ir_analyze_instruction_unreachable(ira, (IrInstructionUnreachable *)instruction);
        case IrInstructionIdPhi:
            return ir_analyze_instruction_phi(ira, (IrInstructionPhi *)instruction);
        case IrInstructionIdTypeOf:
            return ir_analyze_instruction_typeof(ira, (IrInstructionTypeOf *)instruction);
        case IrInstructionIdToPtrType:
            return ir_analyze_instruction_to_ptr_type(ira, (IrInstructionToPtrType *)instruction);
        case IrInstructionIdPtrTypeChild:
            return ir_analyze_instruction_ptr_type_child(ira, (IrInstructionPtrTypeChild *)instruction);
        case IrInstructionIdSetFnTest:
            return ir_analyze_instruction_set_fn_test(ira, (IrInstructionSetFnTest *)instruction);
        case IrInstructionIdSliceType:
            return ir_analyze_instruction_slice_type(ira, (IrInstructionSliceType *)instruction);
        case IrInstructionIdSwitchBr:
        case IrInstructionIdCast:
        case IrInstructionIdContainerInitList:
        case IrInstructionIdContainerInitFields:
        case IrInstructionIdStructFieldPtr:
        case IrInstructionIdArrayType:
            zig_panic("TODO analyze more instructions");
    }
    zig_unreachable();
}

static TypeTableEntry *ir_analyze_instruction(IrAnalyze *ira, IrInstruction *instruction) {
    TypeTableEntry *instruction_type = ir_analyze_instruction_nocast(ira, instruction);
    instruction->type_entry = instruction_type;
    if (instruction->other) {
        instruction->other->type_entry = instruction_type;
    } else {
        assert(instruction_type->id == TypeTableEntryIdInvalid ||
               instruction_type->id == TypeTableEntryIdUnreachable);
        instruction->other = instruction;
    }
    return instruction_type;
}

// This function attempts to evaluate IR code while doing type checking and other analysis.
// It emits a new IrExecutable which is partially evaluated IR code.
TypeTableEntry *ir_analyze(CodeGen *codegen, IrExecutable *old_exec, IrExecutable *new_exec,
        TypeTableEntry *expected_type, AstNode *expected_type_source_node)
{
    IrAnalyze ir_analyze_data = {};
    IrAnalyze *ira = &ir_analyze_data;
    ira->codegen = codegen;
    ira->explicit_return_type = expected_type;

    ira->old_irb.codegen = codegen;
    ira->old_irb.exec = old_exec;

    ira->new_irb.codegen = codegen;
    ira->new_irb.exec = new_exec;

    ira->exec_context.mem_slot_count = ira->old_irb.exec->mem_slot_count;
    ira->exec_context.mem_slot_list = allocate<ConstExprValue>(ira->exec_context.mem_slot_count);

    IrBasicBlock *old_entry_bb = ira->old_irb.exec->basic_block_list.at(0);
    IrBasicBlock *new_entry_bb = ir_get_new_bb(ira, old_entry_bb);
    ir_ref_bb(new_entry_bb);
    ira->old_irb.current_basic_block = old_entry_bb;
    ira->new_irb.current_basic_block = new_entry_bb;
    ira->block_queue_index = 0;
    ira->instruction_index = 0;

    while (ira->block_queue_index < ira->old_bb_queue.length) {
        IrInstruction *old_instruction = ira->old_irb.current_basic_block->instruction_list.at(ira->instruction_index);

        if (old_instruction->ref_count == 0 && !ir_has_side_effects(old_instruction)) {
            ira->instruction_index += 1;
            continue;
        }

        TypeTableEntry *return_type = ir_analyze_instruction(ira, old_instruction);

        // unreachable instructions do their own control flow.
        if (return_type->id == TypeTableEntryIdUnreachable)
            continue;

        ira->instruction_index += 1;
    }

    return ir_resolve_peer_types(ira, expected_type_source_node, ira->implicit_return_type_list.items,
            ira->implicit_return_type_list.length);
}

bool ir_has_side_effects(IrInstruction *instruction) {
    switch (instruction->id) {
        case IrInstructionIdInvalid:
            zig_unreachable();
        case IrInstructionIdBr:
        case IrInstructionIdCondBr:
        case IrInstructionIdSwitchBr:
        case IrInstructionIdDeclVar:
        case IrInstructionIdStorePtr:
        case IrInstructionIdCall:
        case IrInstructionIdReturn:
        case IrInstructionIdUnreachable:
        case IrInstructionIdSetFnTest:
            return true;
        case IrInstructionIdPhi:
        case IrInstructionIdUnOp:
        case IrInstructionIdBinOp:
        case IrInstructionIdLoadPtr:
        case IrInstructionIdConst:
        case IrInstructionIdCast:
        case IrInstructionIdContainerInitList:
        case IrInstructionIdContainerInitFields:
        case IrInstructionIdFieldPtr:
        case IrInstructionIdElemPtr:
        case IrInstructionIdVarPtr:
        case IrInstructionIdTypeOf:
        case IrInstructionIdToPtrType:
        case IrInstructionIdPtrTypeChild:
        case IrInstructionIdReadField:
        case IrInstructionIdStructFieldPtr:
        case IrInstructionIdArrayType:
        case IrInstructionIdSliceType:
            return false;
    }
    zig_unreachable();
}

IrInstruction *ir_exec_const_result(IrExecutable *exec) {
    if (exec->basic_block_list.length != 1)
        return nullptr;

    IrBasicBlock *bb = exec->basic_block_list.at(0);
    if (bb->instruction_list.length != 1)
        return nullptr;

    IrInstruction *only_inst = bb->instruction_list.at(0);
    if (only_inst->id != IrInstructionIdReturn)
        return nullptr;

    IrInstructionReturn *ret_inst = (IrInstructionReturn *)only_inst;
    IrInstruction *value = ret_inst->value;
    assert(value->static_value.ok);
    return value;
}

//static TypeTableEntry *analyze_min_max_value(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        AstNode *node, const char *err_format, bool is_max)
//{
//    assert(node->type == NodeTypeFnCallExpr);
//    assert(node->data.fn_call_expr.params.length == 1);
//
//    AstNode *type_node = node->data.fn_call_expr.params.at(0);
//    TypeTableEntry *type_entry = analyze_type_expr(g, import, context, type_node);
//
//    if (type_entry->id == TypeTableEntryIdInvalid) {
//        return g->builtin_types.entry_invalid;
//    } else if (type_entry->id == TypeTableEntryIdInt) {
//        eval_min_max_value(g, type_entry, &get_resolved_expr(node)->const_val, is_max);
//        return g->builtin_types.entry_num_lit_int;
//    } else if (type_entry->id == TypeTableEntryIdFloat) {
//        eval_min_max_value(g, type_entry, &get_resolved_expr(node)->const_val, is_max);
//        return g->builtin_types.entry_num_lit_float;
//    } else if (type_entry->id == TypeTableEntryIdBool) {
//        eval_min_max_value(g, type_entry, &get_resolved_expr(node)->const_val, is_max);
//        return type_entry;
//    } else {
//        add_node_error(g, node,
//                buf_sprintf(err_format, buf_ptr(&type_entry->name)));
//        return g->builtin_types.entry_invalid;
//    }
//}

//static TypeTableEntry *analyze_import(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        AstNode *node)
//{
//    assert(node->type == NodeTypeFnCallExpr);
//
//    if (context->fn_entry) {
//        add_node_error(g, node, buf_sprintf("@import invalid inside function bodies"));
//        return g->builtin_types.entry_invalid;
//    }
//
//    AstNode *first_param_node = node->data.fn_call_expr.params.at(0);
//    Buf *import_target_str = resolve_const_expr_str(g, import, context, first_param_node->parent_field);
//    if (!import_target_str) {
//        return g->builtin_types.entry_invalid;
//    }
//
//    Buf *import_target_path;
//    Buf *search_dir;
//    assert(import->package);
//    PackageTableEntry *target_package;
//    auto package_entry = import->package->package_table.maybe_get(import_target_str);
//    if (package_entry) {
//        target_package = package_entry->value;
//        import_target_path = &target_package->root_src_path;
//        search_dir = &target_package->root_src_dir;
//    } else {
//        // try it as a filename
//        target_package = import->package;
//        import_target_path = import_target_str;
//        search_dir = &import->package->root_src_dir;
//    }
//
//    Buf full_path = BUF_INIT;
//    os_path_join(search_dir, import_target_path, &full_path);
//
//    Buf *import_code = buf_alloc();
//    Buf *abs_full_path = buf_alloc();
//    int err;
//    if ((err = os_path_real(&full_path, abs_full_path))) {
//        if (err == ErrorFileNotFound) {
//            add_node_error(g, node,
//                    buf_sprintf("unable to find '%s'", buf_ptr(import_target_path)));
//            return g->builtin_types.entry_invalid;
//        } else {
//            g->error_during_imports = true;
//            add_node_error(g, node,
//                    buf_sprintf("unable to open '%s': %s", buf_ptr(&full_path), err_str(err)));
//            return g->builtin_types.entry_invalid;
//        }
//    }
//
//    auto import_entry = g->import_table.maybe_get(abs_full_path);
//    if (import_entry) {
//        return resolve_expr_const_val_as_import(g, node, import_entry->value);
//    }
//
//    if ((err = os_fetch_file_path(abs_full_path, import_code))) {
//        if (err == ErrorFileNotFound) {
//            add_node_error(g, node,
//                    buf_sprintf("unable to find '%s'", buf_ptr(import_target_path)));
//            return g->builtin_types.entry_invalid;
//        } else {
//            add_node_error(g, node,
//                    buf_sprintf("unable to open '%s': %s", buf_ptr(&full_path), err_str(err)));
//            return g->builtin_types.entry_invalid;
//        }
//    }
//    ImportTableEntry *target_import = add_source_file(g, target_package,
//            abs_full_path, search_dir, import_target_path, import_code);
//
//    scan_decls(g, target_import, target_import->block_context, target_import->root);
//
//    return resolve_expr_const_val_as_import(g, node, target_import);
//}
//
//static TypeTableEntry *analyze_c_import(CodeGen *g, ImportTableEntry *parent_import,
//        BlockContext *parent_context, AstNode *node)
//{
//    assert(node->type == NodeTypeFnCallExpr);
//
//    if (parent_context->fn_entry) {
//        add_node_error(g, node, buf_sprintf("@c_import invalid inside function bodies"));
//        return g->builtin_types.entry_invalid;
//    }
//
//    AstNode *block_node = node->data.fn_call_expr.params.at(0);
//
//    BlockContext *child_context = new_block_context(node, parent_context);
//    child_context->c_import_buf = buf_alloc();
//
//    TypeTableEntry *resolved_type = analyze_expression(g, parent_import, child_context,
//            g->builtin_types.entry_void, block_node);
//
//    if (resolved_type->id == TypeTableEntryIdInvalid) {
//        return resolved_type;
//    }
//
//    find_libc_include_path(g);
//
//    ImportTableEntry *child_import = allocate<ImportTableEntry>(1);
//    child_import->c_import_node = node;
//
//    ZigList<ErrorMsg *> errors = {0};
//
//    int err;
//    if ((err = parse_h_buf(child_import, &errors, child_context->c_import_buf, g, node))) {
//        zig_panic("unable to parse h file: %s\n", err_str(err));
//    }
//
//    if (errors.length > 0) {
//        ErrorMsg *parent_err_msg = add_node_error(g, node, buf_sprintf("C import failed"));
//        for (size_t i = 0; i < errors.length; i += 1) {
//            ErrorMsg *err_msg = errors.at(i);
//            err_msg_add_note(parent_err_msg, err_msg);
//        }
//
//        return g->builtin_types.entry_invalid;
//    }
//
//    if (g->verbose) {
//        fprintf(stderr, "\nc_import:\n");
//        fprintf(stderr, "-----------\n");
//        ast_render(stderr, child_import->root, 4);
//    }
//
//    child_import->di_file = parent_import->di_file;
//    child_import->block_context = new_block_context(child_import->root, nullptr);
//
//    scan_decls(g, child_import, child_import->block_context, child_import->root);
//    return resolve_expr_const_val_as_import(g, node, child_import);
//}
//
//static TypeTableEntry *analyze_err_name(CodeGen *g, ImportTableEntry *import,
//        BlockContext *context, AstNode *node)
//{
//    assert(node->type == NodeTypeFnCallExpr);
//
//    AstNode *err_value = node->data.fn_call_expr.params.at(0);
//    TypeTableEntry *resolved_type = analyze_expression(g, import, context,
//            g->builtin_types.entry_pure_error, err_value);
//
//    if (resolved_type->id == TypeTableEntryIdInvalid) {
//        return resolved_type;
//    }
//
//    g->generate_error_name_table = true;
//
//    TypeTableEntry *str_type = get_slice_type(g, g->builtin_types.entry_u8, true);
//    return str_type;
//}
//
//static TypeTableEntry *analyze_embed_file(CodeGen *g, ImportTableEntry *import,
//        BlockContext *context, AstNode *node)
//{
//    assert(node->type == NodeTypeFnCallExpr);
//
//    AstNode **first_param_node = &node->data.fn_call_expr.params.at(0);
//    Buf *rel_file_path = resolve_const_expr_str(g, import, context, first_param_node);
//    if (!rel_file_path) {
//        return g->builtin_types.entry_invalid;
//    }
//
//    // figure out absolute path to resource
//    Buf source_dir_path = BUF_INIT;
//    os_path_dirname(import->path, &source_dir_path);
//
//    Buf file_path = BUF_INIT;
//    os_path_resolve(&source_dir_path, rel_file_path, &file_path);
//
//    // load from file system into const expr
//    Buf file_contents = BUF_INIT;
//    int err;
//    if ((err = os_fetch_file_path(&file_path, &file_contents))) {
//        if (err == ErrorFileNotFound) {
//            add_node_error(g, node,
//                    buf_sprintf("unable to find '%s'", buf_ptr(&file_path)));
//            return g->builtin_types.entry_invalid;
//        } else {
//            add_node_error(g, node,
//                    buf_sprintf("unable to open '%s': %s", buf_ptr(&file_path), err_str(err)));
//            return g->builtin_types.entry_invalid;
//        }
//    }
//
//    // TODO add dependency on the file we embedded so that we know if it changes
//    // we'll have to invalidate the cache
//
//    return resolve_expr_const_val_as_string_lit(g, node, &file_contents);
//}
//
//static TypeTableEntry *analyze_cmpxchg(CodeGen *g, ImportTableEntry *import,
//        BlockContext *context, AstNode *node)
//{
//    assert(node->type == NodeTypeFnCallExpr);
//
//    AstNode **ptr_arg = &node->data.fn_call_expr.params.at(0);
//    AstNode **cmp_arg = &node->data.fn_call_expr.params.at(1);
//    AstNode **new_arg = &node->data.fn_call_expr.params.at(2);
//    AstNode **success_order_arg = &node->data.fn_call_expr.params.at(3);
//    AstNode **failure_order_arg = &node->data.fn_call_expr.params.at(4);
//
//    TypeTableEntry *ptr_type = analyze_expression(g, import, context, nullptr, *ptr_arg);
//    if (ptr_type->id == TypeTableEntryIdInvalid) {
//        return g->builtin_types.entry_invalid;
//    } else if (ptr_type->id != TypeTableEntryIdPointer) {
//        add_node_error(g, *ptr_arg,
//            buf_sprintf("expected pointer argument, got '%s'", buf_ptr(&ptr_type->name)));
//        return g->builtin_types.entry_invalid;
//    }
//
//    TypeTableEntry *child_type = ptr_type->data.pointer.child_type;
//    TypeTableEntry *cmp_type = analyze_expression(g, import, context, child_type, *cmp_arg);
//    TypeTableEntry *new_type = analyze_expression(g, import, context, child_type, *new_arg);
//
//    TypeTableEntry *success_order_type = analyze_expression(g, import, context,
//            g->builtin_types.entry_atomic_order_enum, *success_order_arg);
//    TypeTableEntry *failure_order_type = analyze_expression(g, import, context,
//            g->builtin_types.entry_atomic_order_enum, *failure_order_arg);
//
//    if (cmp_type->id == TypeTableEntryIdInvalid ||
//        new_type->id == TypeTableEntryIdInvalid ||
//        success_order_type->id == TypeTableEntryIdInvalid ||
//        failure_order_type->id == TypeTableEntryIdInvalid)
//    {
//        return g->builtin_types.entry_invalid;
//    }
//
//    ConstExprValue *success_order_val = &get_resolved_expr(*success_order_arg)->const_val;
//    ConstExprValue *failure_order_val = &get_resolved_expr(*failure_order_arg)->const_val;
//    if (!success_order_val->ok) {
//        add_node_error(g, *success_order_arg, buf_sprintf("unable to evaluate constant expression"));
//        return g->builtin_types.entry_invalid;
//    } else if (!failure_order_val->ok) {
//        add_node_error(g, *failure_order_arg, buf_sprintf("unable to evaluate constant expression"));
//        return g->builtin_types.entry_invalid;
//    }
//
//    if (success_order_val->data.x_enum.tag < AtomicOrderMonotonic) {
//        add_node_error(g, *success_order_arg,
//                buf_sprintf("success atomic ordering must be Monotonic or stricter"));
//        return g->builtin_types.entry_invalid;
//    }
//    if (failure_order_val->data.x_enum.tag < AtomicOrderMonotonic) {
//        add_node_error(g, *failure_order_arg,
//                buf_sprintf("failure atomic ordering must be Monotonic or stricter"));
//        return g->builtin_types.entry_invalid;
//    }
//    if (failure_order_val->data.x_enum.tag > success_order_val->data.x_enum.tag) {
//        add_node_error(g, *failure_order_arg,
//                buf_sprintf("failure atomic ordering must be no stricter than success"));
//        return g->builtin_types.entry_invalid;
//    }
//    if (failure_order_val->data.x_enum.tag == AtomicOrderRelease ||
//        failure_order_val->data.x_enum.tag == AtomicOrderAcqRel)
//    {
//        add_node_error(g, *failure_order_arg,
//                buf_sprintf("failure atomic ordering must not be Release or AcqRel"));
//        return g->builtin_types.entry_invalid;
//    }
//
//    return g->builtin_types.entry_bool;
//}
//
//static TypeTableEntry *analyze_fence(CodeGen *g, ImportTableEntry *import,
//        BlockContext *context, AstNode *node)
//{
//    assert(node->type == NodeTypeFnCallExpr);
//
//    AstNode **atomic_order_arg = &node->data.fn_call_expr.params.at(0);
//    TypeTableEntry *atomic_order_type = analyze_expression(g, import, context,
//            g->builtin_types.entry_atomic_order_enum, *atomic_order_arg);
//
//    if (atomic_order_type->id == TypeTableEntryIdInvalid) {
//        return g->builtin_types.entry_invalid;
//    }
//
//    ConstExprValue *atomic_order_val = &get_resolved_expr(*atomic_order_arg)->const_val;
//
//    if (!atomic_order_val->ok) {
//        add_node_error(g, *atomic_order_arg, buf_sprintf("unable to evaluate constant expression"));
//        return g->builtin_types.entry_invalid;
//    }
//
//    return g->builtin_types.entry_void;
//}
//
//static TypeTableEntry *analyze_div_exact(CodeGen *g, ImportTableEntry *import,
//        BlockContext *context, AstNode *node)
//{
//    assert(node->type == NodeTypeFnCallExpr);
//
//    AstNode **op1 = &node->data.fn_call_expr.params.at(0);
//    AstNode **op2 = &node->data.fn_call_expr.params.at(1);
//
//    TypeTableEntry *op1_type = analyze_expression(g, import, context, nullptr, *op1);
//    TypeTableEntry *op2_type = analyze_expression(g, import, context, nullptr, *op2);
//
//    AstNode *op_nodes[] = {*op1, *op2};
//    TypeTableEntry *op_types[] = {op1_type, op2_type};
//    TypeTableEntry *result_type = resolve_peer_type_compatibility(g, import, context, node,
//            op_nodes, op_types, 2);
//
//    if (result_type->id == TypeTableEntryIdInvalid) {
//        return g->builtin_types.entry_invalid;
//    } else if (result_type->id == TypeTableEntryIdInt) {
//        return result_type;
//    } else if (result_type->id == TypeTableEntryIdNumLitInt) {
//        // check for division by zero
//        // check for non exact division
//        zig_panic("TODO");
//    } else {
//        add_node_error(g, node,
//                buf_sprintf("expected integer type, got '%s'", buf_ptr(&result_type->name)));
//        return g->builtin_types.entry_invalid;
//    }
//}
//
//static TypeTableEntry *analyze_truncate(CodeGen *g, ImportTableEntry *import,
//        BlockContext *context, AstNode *node)
//{
//    assert(node->type == NodeTypeFnCallExpr);
//
//    AstNode **op1 = &node->data.fn_call_expr.params.at(0);
//    AstNode **op2 = &node->data.fn_call_expr.params.at(1);
//
//    TypeTableEntry *dest_type = analyze_type_expr(g, import, context, *op1);
//    TypeTableEntry *src_type = analyze_expression(g, import, context, nullptr, *op2);
//
//    if (dest_type->id == TypeTableEntryIdInvalid || src_type->id == TypeTableEntryIdInvalid) {
//        return g->builtin_types.entry_invalid;
//    } else if (dest_type->id != TypeTableEntryIdInt) {
//        add_node_error(g, *op1,
//                buf_sprintf("expected integer type, got '%s'", buf_ptr(&dest_type->name)));
//        return g->builtin_types.entry_invalid;
//    } else if (src_type->id != TypeTableEntryIdInt) {
//        add_node_error(g, *op2,
//                buf_sprintf("expected integer type, got '%s'", buf_ptr(&src_type->name)));
//        return g->builtin_types.entry_invalid;
//    } else if (src_type->data.integral.is_signed != dest_type->data.integral.is_signed) {
//        const char *sign_str = dest_type->data.integral.is_signed ? "signed" : "unsigned";
//        add_node_error(g, *op2,
//                buf_sprintf("expected %s integer type, got '%s'", sign_str, buf_ptr(&src_type->name)));
//        return g->builtin_types.entry_invalid;
//    } else if (src_type->data.integral.bit_count <= dest_type->data.integral.bit_count) {
//        add_node_error(g, *op2,
//                buf_sprintf("type '%s' has same or fewer bits than destination type '%s'",
//                    buf_ptr(&src_type->name), buf_ptr(&dest_type->name)));
//        return g->builtin_types.entry_invalid;
//    }
//
//    // TODO const expr eval
//
//    return dest_type;
//}
//
//static TypeTableEntry *analyze_compile_err(CodeGen *g, ImportTableEntry *import,
//        BlockContext *context, AstNode *node)
//{
//    AstNode *first_param_node = node->data.fn_call_expr.params.at(0);
//    Buf *err_msg = resolve_const_expr_str(g, import, context, first_param_node->parent_field);
//    if (!err_msg) {
//        return g->builtin_types.entry_invalid;
//    }
//
//    add_node_error(g, node, err_msg);
//
//    return g->builtin_types.entry_invalid;
//}
//
//static TypeTableEntry *analyze_int_type(CodeGen *g, ImportTableEntry *import,
//        BlockContext *context, AstNode *node)
//{
//    AstNode **is_signed_node = &node->data.fn_call_expr.params.at(0);
//    AstNode **bit_count_node = &node->data.fn_call_expr.params.at(1);
//
//    TypeTableEntry *bool_type = g->builtin_types.entry_bool;
//    TypeTableEntry *usize_type = g->builtin_types.entry_usize;
//    TypeTableEntry *is_signed_type = analyze_expression(g, import, context, bool_type, *is_signed_node);
//    TypeTableEntry *bit_count_type = analyze_expression(g, import, context, usize_type, *bit_count_node);
//
//    if (is_signed_type->id == TypeTableEntryIdInvalid ||
//        bit_count_type->id == TypeTableEntryIdInvalid)
//    {
//        return g->builtin_types.entry_invalid;
//    }
//
//    ConstExprValue *is_signed_val = &get_resolved_expr(*is_signed_node)->const_val;
//    ConstExprValue *bit_count_val = &get_resolved_expr(*bit_count_node)->const_val;
//
//    AstNode *bad_node = nullptr;
//    if (!is_signed_val->ok) {
//        bad_node = *is_signed_node;
//    } else if (!bit_count_val->ok) {
//        bad_node = *bit_count_node;
//    }
//    if (bad_node) {
//        add_node_error(g, bad_node, buf_sprintf("unable to evaluate constant expression"));
//        return g->builtin_types.entry_invalid;
//    }
//
//    bool depends_on_compile_var = is_signed_val->depends_on_compile_var || bit_count_val->depends_on_compile_var;
//
//    TypeTableEntry *int_type = get_int_type(g, is_signed_val->data.x_bool,
//            bit_count_val->data.x_bignum.data.x_uint);
//    return resolve_expr_const_val_as_type(g, node, int_type, depends_on_compile_var);
//
//}
//
//static TypeTableEntry *analyze_set_fn_no_inline(CodeGen *g, ImportTableEntry *import,
//        BlockContext *context, AstNode *node)
//{
//    AstNode **fn_node = &node->data.fn_call_expr.params.at(0);
//    AstNode **value_node = &node->data.fn_call_expr.params.at(1);
//
//    FnTableEntry *fn_entry = resolve_const_expr_fn(g, import, context, fn_node);
//    if (!fn_entry) {
//        return g->builtin_types.entry_invalid;
//    }
//
//    bool is_noinline;
//    bool ok = resolve_const_expr_bool(g, import, context, value_node, &is_noinline);
//    if (!ok) {
//        return g->builtin_types.entry_invalid;
//    }
//
//    if (fn_entry->fn_no_inline_set_node) {
//        ErrorMsg *msg = add_node_error(g, node, buf_sprintf("function no inline attribute set twice"));
//        add_error_note(g, msg, fn_entry->fn_no_inline_set_node, buf_sprintf("first set here"));
//        return g->builtin_types.entry_invalid;
//    }
//    fn_entry->fn_no_inline_set_node = node;
//
//    if (fn_entry->fn_inline == FnInlineAlways) {
//        add_node_error(g, node, buf_sprintf("function is both inline and noinline"));
//        fn_entry->proto_node->data.fn_proto.skip = true;
//        return g->builtin_types.entry_invalid;
//    } else if (is_noinline) {
//        fn_entry->fn_inline = FnInlineNever;
//    }
//
//    return g->builtin_types.entry_void;
//}
//
//static TypeTableEntry *analyze_set_fn_static_eval(CodeGen *g, ImportTableEntry *import,
//        BlockContext *context, AstNode *node)
//{
//    AstNode **fn_node = &node->data.fn_call_expr.params.at(0);
//    AstNode **value_node = &node->data.fn_call_expr.params.at(1);
//
//    FnTableEntry *fn_entry = resolve_const_expr_fn(g, import, context, fn_node);
//    if (!fn_entry) {
//        return g->builtin_types.entry_invalid;
//    }
//
//    bool want_static_eval;
//    bool ok = resolve_const_expr_bool(g, import, context, value_node, &want_static_eval);
//    if (!ok) {
//        return g->builtin_types.entry_invalid;
//    }
//
//    if (fn_entry->fn_static_eval_set_node) {
//        ErrorMsg *msg = add_node_error(g, node, buf_sprintf("function static eval attribute set twice"));
//        add_error_note(g, msg, fn_entry->fn_static_eval_set_node, buf_sprintf("first set here"));
//        return g->builtin_types.entry_invalid;
//    }
//    fn_entry->fn_static_eval_set_node = node;
//
//    if (want_static_eval && !context->fn_entry->is_pure) {
//        add_node_error(g, node, buf_sprintf("attribute appears too late within function"));
//        return g->builtin_types.entry_invalid;
//    }
//
//    if (want_static_eval) {
//        fn_entry->want_pure = WantPureTrue;
//        fn_entry->want_pure_attr_node = node;
//    } else {
//        fn_entry->want_pure = WantPureFalse;
//        fn_entry->is_pure = false;
//    }
//
//    return g->builtin_types.entry_void;
//}
//
//static TypeTableEntry *analyze_set_fn_visible(CodeGen *g, ImportTableEntry *import,
//        BlockContext *context, AstNode *node)
//{
//    AstNode **fn_node = &node->data.fn_call_expr.params.at(0);
//    AstNode **value_node = &node->data.fn_call_expr.params.at(1);
//
//    FnTableEntry *fn_entry = resolve_const_expr_fn(g, import, context, fn_node);
//    if (!fn_entry) {
//        return g->builtin_types.entry_invalid;
//    }
//
//    bool want_export;
//    bool ok = resolve_const_expr_bool(g, import, context, value_node, &want_export);
//    if (!ok) {
//        return g->builtin_types.entry_invalid;
//    }
//
//    if (fn_entry->fn_export_set_node) {
//        ErrorMsg *msg = add_node_error(g, node, buf_sprintf("function visibility set twice"));
//        add_error_note(g, msg, fn_entry->fn_export_set_node, buf_sprintf("first set here"));
//        return g->builtin_types.entry_invalid;
//    }
//    fn_entry->fn_export_set_node = node;
//
//    AstNodeFnProto *fn_proto = &fn_entry->proto_node->data.fn_proto;
//    if (fn_proto->top_level_decl.visib_mod != VisibModExport) {
//        ErrorMsg *msg = add_node_error(g, node,
//            buf_sprintf("function must be marked export to set function visibility"));
//        add_error_note(g, msg, fn_entry->proto_node, buf_sprintf("function declared here"));
//        return g->builtin_types.entry_void;
//    }
//    if (!want_export) {
//        fn_proto->top_level_decl.visib_mod = VisibModPub;
//    }
//
//    return g->builtin_types.entry_void;
//}
//
//static TypeTableEntry *analyze_set_debug_safety(CodeGen *g, ImportTableEntry *import,
//        BlockContext *parent_context, AstNode *node)
//{
//    AstNode **target_node = &node->data.fn_call_expr.params.at(0);
//    AstNode **value_node = &node->data.fn_call_expr.params.at(1);
//
//    TypeTableEntry *target_type = analyze_expression(g, import, parent_context, nullptr, *target_node);
//    BlockContext *target_context;
//    ConstExprValue *const_val = &get_resolved_expr(*target_node)->const_val;
//    if (target_type->id == TypeTableEntryIdInvalid) {
//        return g->builtin_types.entry_invalid;
//    }
//    if (!const_val->ok) {
//        add_node_error(g, *target_node, buf_sprintf("unable to evaluate constant expression"));
//        return g->builtin_types.entry_invalid;
//    }
//    if (target_type->id == TypeTableEntryIdBlock) {
//        target_context = const_val->data.x_block;
//    } else if (target_type->id == TypeTableEntryIdFn) {
//        target_context = const_val->data.x_fn->fn_def_node->data.fn_def.block_context;
//    } else if (target_type->id == TypeTableEntryIdMetaType) {
//        TypeTableEntry *type_arg = const_val->data.x_type;
//        if (type_arg->id == TypeTableEntryIdStruct) {
//            target_context = type_arg->data.structure.block_context;
//        } else if (type_arg->id == TypeTableEntryIdEnum) {
//            target_context = type_arg->data.enumeration.block_context;
//        } else if (type_arg->id == TypeTableEntryIdUnion) {
//            target_context = type_arg->data.unionation.block_context;
//        } else {
//            add_node_error(g, *target_node,
//                buf_sprintf("expected scope reference, got type '%s'", buf_ptr(&type_arg->name)));
//            return g->builtin_types.entry_invalid;
//        }
//    } else {
//        add_node_error(g, *target_node,
//            buf_sprintf("expected scope reference, got type '%s'", buf_ptr(&target_type->name)));
//        return g->builtin_types.entry_invalid;
//    }
//
//    bool want_debug_safety;
//    bool ok = resolve_const_expr_bool(g, import, parent_context, value_node, &want_debug_safety);
//    if (!ok) {
//        return g->builtin_types.entry_invalid;
//    }
//
//    if (target_context->safety_set_node) {
//        ErrorMsg *msg = add_node_error(g, node, buf_sprintf("debug safety for scope set twice"));
//        add_error_note(g, msg, target_context->safety_set_node, buf_sprintf("first set here"));
//        return g->builtin_types.entry_invalid;
//    }
//    target_context->safety_set_node = node;
//
//    target_context->safety_off = !want_debug_safety;
//
//    return g->builtin_types.entry_void;
//}


//static TypeTableEntry *analyze_builtin_fn_call_expr(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        TypeTableEntry *expected_type, AstNode *node)
//{
//
//    switch (builtin_fn->id) {
//        case BuiltinFnIdInvalid:
//            zig_unreachable();
//        case BuiltinFnIdAddWithOverflow:
//        case BuiltinFnIdSubWithOverflow:
//        case BuiltinFnIdMulWithOverflow:
//        case BuiltinFnIdShlWithOverflow:
//            {
//                AstNode *type_node = node->data.fn_call_expr.params.at(0);
//                TypeTableEntry *int_type = analyze_type_expr(g, import, context, type_node);
//                if (int_type->id == TypeTableEntryIdInvalid) {
//                    return g->builtin_types.entry_bool;
//                } else if (int_type->id == TypeTableEntryIdInt) {
//                    AstNode *op1_node = node->data.fn_call_expr.params.at(1);
//                    AstNode *op2_node = node->data.fn_call_expr.params.at(2);
//                    AstNode *result_node = node->data.fn_call_expr.params.at(3);
//
//                    analyze_expression(g, import, context, int_type, op1_node);
//                    analyze_expression(g, import, context, int_type, op2_node);
//                    analyze_expression(g, import, context, get_pointer_to_type(g, int_type, false),
//                            result_node);
//                } else {
//                    add_node_error(g, type_node,
//                        buf_sprintf("expected integer type, got '%s'", buf_ptr(&int_type->name)));
//                }
//
//                // TODO constant expression evaluation
//
//                return g->builtin_types.entry_bool;
//            }
//        case BuiltinFnIdMemcpy:
//            {
//                AstNode *dest_node = node->data.fn_call_expr.params.at(0);
//                AstNode *src_node = node->data.fn_call_expr.params.at(1);
//                AstNode *len_node = node->data.fn_call_expr.params.at(2);
//                TypeTableEntry *dest_type = analyze_expression(g, import, context, nullptr, dest_node);
//                TypeTableEntry *src_type = analyze_expression(g, import, context, nullptr, src_node);
//                analyze_expression(g, import, context, builtin_fn->param_types[2], len_node);
//
//                if (dest_type->id != TypeTableEntryIdInvalid &&
//                    dest_type->id != TypeTableEntryIdPointer)
//                {
//                    add_node_error(g, dest_node,
//                            buf_sprintf("expected pointer argument, got '%s'", buf_ptr(&dest_type->name)));
//                }
//
//                if (src_type->id != TypeTableEntryIdInvalid &&
//                    src_type->id != TypeTableEntryIdPointer)
//                {
//                    add_node_error(g, src_node,
//                            buf_sprintf("expected pointer argument, got '%s'", buf_ptr(&src_type->name)));
//                }
//
//                if (dest_type->id == TypeTableEntryIdPointer &&
//                    src_type->id == TypeTableEntryIdPointer)
//                {
//                    uint64_t dest_align = get_memcpy_align(g, dest_type->data.pointer.child_type);
//                    uint64_t src_align = get_memcpy_align(g, src_type->data.pointer.child_type);
//                    if (dest_align != src_align) {
//                        add_node_error(g, dest_node, buf_sprintf(
//                            "misaligned memcpy, '%s' has alignment '%" PRIu64 ", '%s' has alignment %" PRIu64,
//                                    buf_ptr(&dest_type->name), dest_align,
//                                    buf_ptr(&src_type->name), src_align));
//                    }
//                }
//
//                return builtin_fn->return_type;
//            }
//        case BuiltinFnIdMemset:
//            {
//                AstNode *dest_node = node->data.fn_call_expr.params.at(0);
//                AstNode *char_node = node->data.fn_call_expr.params.at(1);
//                AstNode *len_node = node->data.fn_call_expr.params.at(2);
//                TypeTableEntry *dest_type = analyze_expression(g, import, context, nullptr, dest_node);
//                analyze_expression(g, import, context, builtin_fn->param_types[1], char_node);
//                analyze_expression(g, import, context, builtin_fn->param_types[2], len_node);
//
//                if (dest_type->id != TypeTableEntryIdInvalid &&
//                    dest_type->id != TypeTableEntryIdPointer)
//                {
//                    add_node_error(g, dest_node,
//                            buf_sprintf("expected pointer argument, got '%s'", buf_ptr(&dest_type->name)));
//                }
//
//                return builtin_fn->return_type;
//            }
//        case BuiltinFnIdSizeof:
//            {
//                AstNode *type_node = node->data.fn_call_expr.params.at(0);
//                TypeTableEntry *type_entry = analyze_type_expr(g, import, context, type_node);
//                if (type_entry->id == TypeTableEntryIdInvalid) {
//                    return g->builtin_types.entry_invalid;
//                } else if (type_entry->id == TypeTableEntryIdUnreachable) {
//                    add_node_error(g, first_executing_node(type_node),
//                            buf_sprintf("no size available for type '%s'", buf_ptr(&type_entry->name)));
//                    return g->builtin_types.entry_invalid;
//                } else {
//                    uint64_t size_in_bytes = type_size(g, type_entry);
//                    bool depends_on_compile_var = (type_entry == g->builtin_types.entry_usize ||
//                            type_entry == g->builtin_types.entry_isize);
//                    return resolve_expr_const_val_as_unsigned_num_lit(g, node, expected_type,
//                            size_in_bytes, depends_on_compile_var);
//                }
//            }
//        case BuiltinFnIdAlignof:
//            {
//                AstNode *type_node = node->data.fn_call_expr.params.at(0);
//                TypeTableEntry *type_entry = analyze_type_expr(g, import, context, type_node);
//                if (type_entry->id == TypeTableEntryIdInvalid) {
//                    return g->builtin_types.entry_invalid;
//                } else if (type_entry->id == TypeTableEntryIdUnreachable) {
//                    add_node_error(g, first_executing_node(type_node),
//                            buf_sprintf("no align available for type '%s'", buf_ptr(&type_entry->name)));
//                    return g->builtin_types.entry_invalid;
//                } else {
//                    uint64_t align_in_bytes = LLVMABISizeOfType(g->target_data_ref, type_entry->type_ref);
//                    return resolve_expr_const_val_as_unsigned_num_lit(g, node, expected_type,
//                            align_in_bytes, false);
//                }
//            }
//        case BuiltinFnIdMaxValue:
//            return analyze_min_max_value(g, import, context, node,
//                    "no max value available for type '%s'", true);
//        case BuiltinFnIdMinValue:
//            return analyze_min_max_value(g, import, context, node,
//                    "no min value available for type '%s'", false);
//        case BuiltinFnIdMemberCount:
//            {
//                AstNode *type_node = node->data.fn_call_expr.params.at(0);
//                TypeTableEntry *type_entry = analyze_type_expr(g, import, context, type_node);
//
//                if (type_entry->id == TypeTableEntryIdInvalid) {
//                    return type_entry;
//                } else if (type_entry->id == TypeTableEntryIdEnum) {
//                    uint64_t value_count = type_entry->data.enumeration.src_field_count;
//                    return resolve_expr_const_val_as_unsigned_num_lit(g, node, expected_type,
//                            value_count, false);
//                } else {
//                    add_node_error(g, node,
//                            buf_sprintf("no value count available for type '%s'", buf_ptr(&type_entry->name)));
//                    return g->builtin_types.entry_invalid;
//                }
//            }
//        case BuiltinFnIdCInclude:
//            {
//                if (!context->c_import_buf) {
//                    add_node_error(g, node, buf_sprintf("@c_include valid only in c_import blocks"));
//                    return g->builtin_types.entry_invalid;
//                }
//
//                AstNode **str_node = node->data.fn_call_expr.params.at(0)->parent_field;
//                TypeTableEntry *str_type = get_slice_type(g, g->builtin_types.entry_u8, true);
//                TypeTableEntry *resolved_type = analyze_expression(g, import, context, str_type, *str_node);
//
//                if (resolved_type->id == TypeTableEntryIdInvalid) {
//                    return resolved_type;
//                }
//
//                ConstExprValue *const_str_val = &get_resolved_expr(*str_node)->const_val;
//
//                if (!const_str_val->ok) {
//                    add_node_error(g, *str_node, buf_sprintf("@c_include requires constant expression"));
//                    return g->builtin_types.entry_void;
//                }
//
//                buf_appendf(context->c_import_buf, "#include <");
//                ConstExprValue *ptr_field = const_str_val->data.x_struct.fields[0];
//                uint64_t len = ptr_field->data.x_ptr.len;
//                for (uint64_t i = 0; i < len; i += 1) {
//                    ConstExprValue *char_val = ptr_field->data.x_ptr.ptr[i];
//                    uint64_t big_c = char_val->data.x_bignum.data.x_uint;
//                    assert(big_c <= UINT8_MAX);
//                    uint8_t c = big_c;
//                    buf_append_char(context->c_import_buf, c);
//                }
//                buf_appendf(context->c_import_buf, ">\n");
//
//                return g->builtin_types.entry_void;
//            }
//        case BuiltinFnIdCDefine:
//            zig_panic("TODO");
//        case BuiltinFnIdCUndef:
//            zig_panic("TODO");
//
//        case BuiltinFnIdCompileVar:
//            {
//                AstNode **str_node = node->data.fn_call_expr.params.at(0)->parent_field;
//
//                Buf *var_name = resolve_const_expr_str(g, import, context, str_node);
//                if (!var_name) {
//                    return g->builtin_types.entry_invalid;
//                }
//
//                ConstExprValue *const_val = &get_resolved_expr(node)->const_val;
//                const_val->ok = true;
//                const_val->depends_on_compile_var = true;
//
//                if (buf_eql_str(var_name, "is_big_endian")) {
//                    return resolve_expr_const_val_as_bool(g, node, g->is_big_endian, true);
//                } else if (buf_eql_str(var_name, "is_release")) {
//                    return resolve_expr_const_val_as_bool(g, node, g->is_release_build, true);
//                } else if (buf_eql_str(var_name, "is_test")) {
//                    return resolve_expr_const_val_as_bool(g, node, g->is_test_build, true);
//                } else if (buf_eql_str(var_name, "os")) {
//                    const_val->data.x_enum.tag = g->target_os_index;
//                    return g->builtin_types.entry_os_enum;
//                } else if (buf_eql_str(var_name, "arch")) {
//                    const_val->data.x_enum.tag = g->target_arch_index;
//                    return g->builtin_types.entry_arch_enum;
//                } else if (buf_eql_str(var_name, "environ")) {
//                    const_val->data.x_enum.tag = g->target_environ_index;
//                    return g->builtin_types.entry_environ_enum;
//                } else if (buf_eql_str(var_name, "object_format")) {
//                    const_val->data.x_enum.tag = g->target_oformat_index;
//                    return g->builtin_types.entry_oformat_enum;
//                } else {
//                    add_node_error(g, *str_node,
//                        buf_sprintf("unrecognized compile variable: '%s'", buf_ptr(var_name)));
//                    return g->builtin_types.entry_invalid;
//                }
//            }
//        case BuiltinFnIdConstEval:
//            {
//                AstNode **expr_node = node->data.fn_call_expr.params.at(0)->parent_field;
//                TypeTableEntry *resolved_type = analyze_expression(g, import, context, expected_type, *expr_node);
//                if (resolved_type->id == TypeTableEntryIdInvalid) {
//                    return resolved_type;
//                }
//
//                ConstExprValue *const_expr_val = &get_resolved_expr(*expr_node)->const_val;
//
//                if (!const_expr_val->ok) {
//                    add_node_error(g, *expr_node, buf_sprintf("unable to evaluate constant expression"));
//                    return g->builtin_types.entry_invalid;
//                }
//
//                ConstExprValue *const_val = &get_resolved_expr(node)->const_val;
//                *const_val = *const_expr_val;
//
//                return resolved_type;
//            }
//        case BuiltinFnIdCtz:
//        case BuiltinFnIdClz:
//            {
//                AstNode *type_node = node->data.fn_call_expr.params.at(0);
//                TypeTableEntry *int_type = analyze_type_expr(g, import, context, type_node);
//                if (int_type->id == TypeTableEntryIdInvalid) {
//                    return int_type;
//                } else if (int_type->id == TypeTableEntryIdInt) {
//                    AstNode **expr_node = node->data.fn_call_expr.params.at(1)->parent_field;
//                    TypeTableEntry *resolved_type = analyze_expression(g, import, context, int_type, *expr_node);
//                    if (resolved_type->id == TypeTableEntryIdInvalid) {
//                        return resolved_type;
//                    }
//
//                    // TODO const expr eval
//
//                    return resolved_type;
//                } else {
//                    add_node_error(g, type_node,
//                        buf_sprintf("expected integer type, got '%s'", buf_ptr(&int_type->name)));
//                    return g->builtin_types.entry_invalid;
//                }
//            }
//        case BuiltinFnIdImport:
//            return analyze_import(g, import, context, node);
//        case BuiltinFnIdCImport:
//            return analyze_c_import(g, import, context, node);
//        case BuiltinFnIdErrName:
//            return analyze_err_name(g, import, context, node);
//        case BuiltinFnIdBreakpoint:
//            mark_impure_fn(g, context, node);
//            return g->builtin_types.entry_void;
//        case BuiltinFnIdReturnAddress:
//        case BuiltinFnIdFrameAddress:
//            mark_impure_fn(g, context, node);
//            return builtin_fn->return_type;
//        case BuiltinFnIdEmbedFile:
//            return analyze_embed_file(g, import, context, node);
//        case BuiltinFnIdCmpExchange:
//            return analyze_cmpxchg(g, import, context, node);
//        case BuiltinFnIdFence:
//            return analyze_fence(g, import, context, node);
//        case BuiltinFnIdDivExact:
//            return analyze_div_exact(g, import, context, node);
//        case BuiltinFnIdTruncate:
//            return analyze_truncate(g, import, context, node);
//        case BuiltinFnIdCompileErr:
//            return analyze_compile_err(g, import, context, node);
//        case BuiltinFnIdIntType:
//            return analyze_int_type(g, import, context, node);
//        case BuiltinFnIdSetFnTest:
//            return analyze_set_fn_test(g, import, context, node);
//        case BuiltinFnIdSetFnNoInline:
//            return analyze_set_fn_no_inline(g, import, context, node);
//        case BuiltinFnIdSetFnStaticEval:
//            return analyze_set_fn_static_eval(g, import, context, node);
//        case BuiltinFnIdSetFnVisible:
//            return analyze_set_fn_visible(g, import, context, node);
//        case BuiltinFnIdSetDebugSafety:
//            return analyze_set_debug_safety(g, import, context, node);
//    }
//    zig_unreachable();
//}

//static TypeTableEntry *analyze_container_init_expr(CodeGen *g, ImportTableEntry *import,
//    BlockContext *context, AstNode *node)
//{
//    assert(node->type == NodeTypeContainerInitExpr);
//
//    AstNodeContainerInitExpr *container_init_expr = &node->data.container_init_expr;
//
//    ContainerInitKind kind = container_init_expr->kind;
//
//    if (container_init_expr->type->type == NodeTypeFieldAccessExpr) {
//        container_init_expr->type->data.field_access_expr.container_init_expr_node = node;
//    }
//
//    TypeTableEntry *container_meta_type = analyze_expression(g, import, context, nullptr,
//            container_init_expr->type);
//
//    if (container_meta_type->id == TypeTableEntryIdInvalid) {
//        return g->builtin_types.entry_invalid;
//    }
//
//    if (node->data.container_init_expr.enum_type) {
//        get_resolved_expr(node)->const_val = get_resolved_expr(container_init_expr->type)->const_val;
//        return node->data.container_init_expr.enum_type;
//    }
//
//    TypeTableEntry *container_type = resolve_type(g, container_init_expr->type);
//
//    if (container_type->id == TypeTableEntryIdInvalid) {
//        return container_type;
//    } else if (container_type->id == TypeTableEntryIdStruct &&
//               !container_type->data.structure.is_slice &&
//               (kind == ContainerInitKindStruct || (kind == ContainerInitKindArray &&
//                                                    container_init_expr->entries.length == 0)))
//    {
//        StructValExprCodeGen *codegen = &container_init_expr->resolved_struct_val_expr;
//        codegen->type_entry = container_type;
//        codegen->source_node = node;
//
//
//        size_t expr_field_count = container_init_expr->entries.length;
//        size_t actual_field_count = container_type->data.structure.src_field_count;
//
//        AstNode *non_const_expr_culprit = nullptr;
//
//        size_t *field_use_counts = allocate<size_t>(actual_field_count);
//        ConstExprValue *const_val = &get_resolved_expr(node)->const_val;
//        const_val->ok = true;
//        const_val->data.x_struct.fields = allocate<ConstExprValue*>(actual_field_count);
//        for (size_t i = 0; i < expr_field_count; i += 1) {
//            AstNode *val_field_node = container_init_expr->entries.at(i);
//            assert(val_field_node->type == NodeTypeStructValueField);
//
//            val_field_node->block_context = context;
//
//            TypeStructField *type_field = find_struct_type_field(container_type,
//                    val_field_node->data.struct_val_field.name);
//
//            if (!type_field) {
//                add_node_error(g, val_field_node,
//                    buf_sprintf("no member named '%s' in '%s'",
//                        buf_ptr(val_field_node->data.struct_val_field.name), buf_ptr(&container_type->name)));
//                continue;
//            }
//
//            if (type_field->type_entry->id == TypeTableEntryIdInvalid) {
//                return g->builtin_types.entry_invalid;
//            }
//
//            size_t field_index = type_field->src_index;
//            field_use_counts[field_index] += 1;
//            if (field_use_counts[field_index] > 1) {
//                add_node_error(g, val_field_node, buf_sprintf("duplicate field"));
//                continue;
//            }
//
//            val_field_node->data.struct_val_field.type_struct_field = type_field;
//
//            analyze_expression(g, import, context, type_field->type_entry,
//                    val_field_node->data.struct_val_field.expr);
//
//            if (const_val->ok) {
//                ConstExprValue *field_val =
//                    &get_resolved_expr(val_field_node->data.struct_val_field.expr)->const_val;
//                if (field_val->ok) {
//                    const_val->data.x_struct.fields[field_index] = field_val;
//                    const_val->depends_on_compile_var = const_val->depends_on_compile_var || field_val->depends_on_compile_var;
//                } else {
//                    const_val->ok = false;
//                    non_const_expr_culprit = val_field_node->data.struct_val_field.expr;
//                }
//            }
//        }
//        if (!const_val->ok) {
//            assert(non_const_expr_culprit);
//            if (context->fn_entry) {
//                context->fn_entry->struct_val_expr_alloca_list.append(codegen);
//            } else {
//                add_node_error(g, non_const_expr_culprit, buf_sprintf("unable to evaluate constant expression"));
//            }
//        }
//
//        for (size_t i = 0; i < actual_field_count; i += 1) {
//            if (field_use_counts[i] == 0) {
//                add_node_error(g, node,
//                    buf_sprintf("missing field: '%s'", buf_ptr(container_type->data.structure.fields[i].name)));
//            }
//        }
//        return container_type;
//    } else if (container_type->id == TypeTableEntryIdStruct &&
//               container_type->data.structure.is_slice &&
//               kind == ContainerInitKindArray)
//    {
//        size_t elem_count = container_init_expr->entries.length;
//
//        TypeTableEntry *pointer_type = container_type->data.structure.fields[0].type_entry;
//        assert(pointer_type->id == TypeTableEntryIdPointer);
//        TypeTableEntry *child_type = pointer_type->data.pointer.child_type;
//
//        ConstExprValue *const_val = &get_resolved_expr(node)->const_val;
//        const_val->ok = true;
//        const_val->data.x_array.fields = allocate<ConstExprValue*>(elem_count);
//
//        for (size_t i = 0; i < elem_count; i += 1) {
//            AstNode **elem_node = &container_init_expr->entries.at(i);
//            analyze_expression(g, import, context, child_type, *elem_node);
//
//            if (const_val->ok) {
//                ConstExprValue *elem_const_val = &get_resolved_expr(*elem_node)->const_val;
//                if (elem_const_val->ok) {
//                    const_val->data.x_array.fields[i] = elem_const_val;
//                    const_val->depends_on_compile_var = const_val->depends_on_compile_var ||
//                        elem_const_val->depends_on_compile_var;
//                } else {
//                    const_val->ok = false;
//                }
//            }
//        }
//
//        TypeTableEntry *fixed_size_array_type = get_array_type(g, child_type, elem_count);
//
//        StructValExprCodeGen *codegen = &container_init_expr->resolved_struct_val_expr;
//        codegen->type_entry = fixed_size_array_type;
//        codegen->source_node = node;
//        if (!const_val->ok) {
//            if (!context->fn_entry) {
//                add_node_error(g, node,
//                    buf_sprintf("unable to evaluate constant expression"));
//            } else {
//                context->fn_entry->struct_val_expr_alloca_list.append(codegen);
//            }
//        }
//
//        return fixed_size_array_type;
//    } else if (container_type->id == TypeTableEntryIdArray) {
//        zig_panic("TODO array container init");
//        return container_type;
//    } else if (container_type->id == TypeTableEntryIdVoid) {
//        if (container_init_expr->entries.length != 0) {
//            add_node_error(g, node, buf_sprintf("void expression expects no arguments"));
//            return g->builtin_types.entry_invalid;
//        } else {
//            return resolve_expr_const_val_as_void(g, node);
//        }
//    } else {
//        add_node_error(g, node,
//            buf_sprintf("type '%s' does not support %s initialization syntax",
//                buf_ptr(&container_type->name), err_container_init_syntax_name(kind)));
//        return g->builtin_types.entry_invalid;
//    }
//}



//static TypeTableEntry *analyze_field_access_expr(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        TypeTableEntry *expected_type, AstNode *node)
//{
//    assert(node->type == NodeTypeFieldAccessExpr);
//
//    AstNode **struct_expr_node = &node->data.field_access_expr.struct_expr;
//    TypeTableEntry *struct_type = analyze_expression(g, import, context, nullptr, *struct_expr_node);
//    Buf *field_name = node->data.field_access_expr.field_name;
//
//    if (struct_type->id == TypeTableEntryIdInvalid) {
//        return struct_type;
//    } else if (is_container_ref(struct_type)) {
//        return analyze_container_member_access(g, field_name, node, struct_type);
//    } else if (struct_type->id == TypeTableEntryIdArray) {
//        if (buf_eql_str(field_name, "len")) {
//            return resolve_expr_const_val_as_unsigned_num_lit(g, node, expected_type,
//                    struct_type->data.array.len, false);
//        } else {
//            add_node_error(g, node,
//                buf_sprintf("no member named '%s' in '%s'", buf_ptr(field_name),
//                    buf_ptr(&struct_type->name)));
//            return g->builtin_types.entry_invalid;
//        }
//    } else if (struct_type->id == TypeTableEntryIdMetaType) {
//        TypeTableEntry *child_type = resolve_type(g, *struct_expr_node);
//
//        if (child_type->id == TypeTableEntryIdInvalid) {
//            return g->builtin_types.entry_invalid;
//        } else if (child_type->id == TypeTableEntryIdEnum) {
//            AstNode *container_init_node = node->data.field_access_expr.container_init_expr_node;
//            AstNode *value_node;
//            if (container_init_node) {
//                assert(container_init_node->type == NodeTypeContainerInitExpr);
//                size_t param_count = container_init_node->data.container_init_expr.entries.length;
//                if (param_count > 1) {
//                    AstNode *first_invalid_node = container_init_node->data.container_init_expr.entries.at(1);
//                    add_node_error(g, first_executing_node(first_invalid_node),
//                            buf_sprintf("enum values accept only one parameter"));
//                    return child_type;
//                } else {
//                    if (param_count == 1) {
//                        value_node = container_init_node->data.container_init_expr.entries.at(0);
//                    } else {
//                        value_node = nullptr;
//                    }
//                    container_init_node->data.container_init_expr.enum_type = child_type;
//                }
//            } else {
//                value_node = nullptr;
//            }
//            return analyze_enum_value_expr(g, import, context, node, value_node, child_type, field_name, node);
//        } else if (child_type->id == TypeTableEntryIdStruct) {
//            BlockContext *container_block_context = get_container_block_context(child_type);
//            auto entry = container_block_context->decl_table.maybe_get(field_name);
//            AstNode *decl_node = entry ? entry->value : nullptr;
//            if (decl_node) {
//                bool pointer_only = false;
//                return analyze_decl_ref(g, node, decl_node, pointer_only, context, false);
//            } else {
//                add_node_error(g, node,
//                    buf_sprintf("container '%s' has no member called '%s'",
//                        buf_ptr(&child_type->name), buf_ptr(field_name)));
//                return g->builtin_types.entry_invalid;
//            }
//        } else if (child_type->id == TypeTableEntryIdPureError) {
//            return analyze_error_literal_expr(g, import, context, node, field_name);
//        } else if (child_type->id == TypeTableEntryIdInt) {
//            bool depends_on_compile_var =
//                get_resolved_expr(*struct_expr_node)->const_val.depends_on_compile_var;
//            if (buf_eql_str(field_name, "bit_count")) {
//                return resolve_expr_const_val_as_unsigned_num_lit(g, node, expected_type,
//                        child_type->data.integral.bit_count, depends_on_compile_var);
//            } else if (buf_eql_str(field_name, "is_signed")) {
//                return resolve_expr_const_val_as_bool(g, node, child_type->data.integral.is_signed,
//                        depends_on_compile_var);
//            } else {
//                add_node_error(g, node,
//                    buf_sprintf("type '%s' has no member called '%s'",
//                        buf_ptr(&child_type->name), buf_ptr(field_name)));
//                return g->builtin_types.entry_invalid;
//            }
//        } else {
//            add_node_error(g, node,
//                buf_sprintf("type '%s' does not support field access", buf_ptr(&struct_type->name)));
//            return g->builtin_types.entry_invalid;
//        }
//    } else if (struct_type->id == TypeTableEntryIdNamespace) {
//        ConstExprValue *const_val = &get_resolved_expr(*struct_expr_node)->const_val;
//        assert(const_val->ok);
//        ImportTableEntry *namespace_import = const_val->data.x_import;
//        AstNode *decl_node = find_decl(namespace_import->block_context, field_name);
//        if (!decl_node) {
//            // we must now resolve all the use decls
//            for (size_t i = 0; i < namespace_import->use_decls.length; i += 1) {
//                AstNode *use_decl_node = namespace_import->use_decls.at(i);
//                if (!get_resolved_expr(use_decl_node->data.use.expr)->type_entry) {
//                    preview_use_decl(g, use_decl_node);
//                }
//                resolve_use_decl(g, use_decl_node);
//            }
//            decl_node = find_decl(namespace_import->block_context, field_name);
//        }
//        if (decl_node) {
//            TopLevelDecl *tld = get_as_top_level_decl(decl_node);
//            if (tld->visib_mod == VisibModPrivate && decl_node->owner != import) {
//                ErrorMsg *msg = add_node_error(g, node,
//                    buf_sprintf("'%s' is private", buf_ptr(field_name)));
//                add_error_note(g, msg, decl_node, buf_sprintf("declared here"));
//            }
//            bool pointer_only = false;
//            return analyze_decl_ref(g, node, decl_node, pointer_only, context,
//                    const_val->depends_on_compile_var);
//        } else {
//            const char *import_name = namespace_import->path ? buf_ptr(namespace_import->path) : "(C import)";
//            add_node_error(g, node,
//                buf_sprintf("no member named '%s' in '%s'", buf_ptr(field_name), import_name));
//            return g->builtin_types.entry_invalid;
//        }
//    } else {
//        add_node_error(g, node,
//            buf_sprintf("type '%s' does not support field access", buf_ptr(&struct_type->name)));
//        return g->builtin_types.entry_invalid;
//    }
//}
//

//static TypeTableEntry *analyze_lvalue(CodeGen *g, ImportTableEntry *import, BlockContext *block_context,
//        AstNode *lhs_node, LValPurpose purpose, bool is_ptr_const)
//{
//    TypeTableEntry *expected_rhs_type = nullptr;
//    lhs_node->block_context = block_context;
//    if (lhs_node->type == NodeTypeSymbol) {
//        bool pointer_only = purpose == LValPurposeAddressOf;
//        expected_rhs_type = analyze_symbol_expr(g, import, block_context, nullptr, lhs_node, pointer_only);
//        if (expected_rhs_type->id == TypeTableEntryIdInvalid) {
//            return g->builtin_types.entry_invalid;
//        }
//        if (purpose != LValPurposeAddressOf) {
//            Buf *name = lhs_node->data.symbol_expr.symbol;
//            VariableTableEntry *var = find_variable(g, block_context, name);
//            if (var) {
//                if (var->src_is_const) {
//                    add_node_error(g, lhs_node, buf_sprintf("cannot assign to constant"));
//                    expected_rhs_type = g->builtin_types.entry_invalid;
//                } else {
//                    expected_rhs_type = var->type;
//                    get_resolved_expr(lhs_node)->variable = var;
//                }
//            } else {
//                add_node_error(g, lhs_node,
//                        buf_sprintf("use of undeclared identifier '%s'", buf_ptr(name)));
//                expected_rhs_type = g->builtin_types.entry_invalid;
//            }
//        }
//    } else if (lhs_node->type == NodeTypeArrayAccessExpr) {
//        expected_rhs_type = analyze_array_access_expr(g, import, block_context, lhs_node, purpose);
//    } else if (lhs_node->type == NodeTypeFieldAccessExpr) {
//        expected_rhs_type = analyze_field_access_expr(g, import, block_context, nullptr, lhs_node);
//    } else if (lhs_node->type == NodeTypePrefixOpExpr &&
//            lhs_node->data.prefix_op_expr.prefix_op == PrefixOpDereference)
//    {
//        assert(purpose == LValPurposeAssign);
//        AstNode *target_node = lhs_node->data.prefix_op_expr.primary_expr;
//        TypeTableEntry *type_entry = analyze_expression(g, import, block_context, nullptr, target_node);
//        if (type_entry->id == TypeTableEntryIdInvalid) {
//            expected_rhs_type = type_entry;
//        } else if (type_entry->id == TypeTableEntryIdPointer) {
//            expected_rhs_type = type_entry->data.pointer.child_type;
//        } else {
//            add_node_error(g, target_node,
//                buf_sprintf("indirection requires pointer operand ('%s' invalid)",
//                    buf_ptr(&type_entry->name)));
//            expected_rhs_type = g->builtin_types.entry_invalid;
//        }
//    } else {
//        if (purpose == LValPurposeAssign) {
//            add_node_error(g, lhs_node, buf_sprintf("invalid assignment target"));
//            expected_rhs_type = g->builtin_types.entry_invalid;
//        } else if (purpose == LValPurposeAddressOf) {
//            TypeTableEntry *type_entry = analyze_expression(g, import, block_context, nullptr, lhs_node);
//            if (type_entry->id == TypeTableEntryIdInvalid) {
//                expected_rhs_type = g->builtin_types.entry_invalid;
//            } else if (type_entry->id == TypeTableEntryIdMetaType) {
//                expected_rhs_type = type_entry;
//            } else {
//                add_node_error(g, lhs_node, buf_sprintf("invalid addressof target"));
//                expected_rhs_type = g->builtin_types.entry_invalid;
//            }
//        }
//    }
//    assert(expected_rhs_type);
//    return expected_rhs_type;
//}



//static TypeTableEntry *analyze_bin_op_expr(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        TypeTableEntry *expected_type, AstNode *node)
//{
//    assert(node->type == NodeTypeBinOpExpr);
//    BinOpType bin_op_type = node->data.bin_op_expr.bin_op;
//    switch (bin_op_type) {
//        case BinOpTypeAssign:
//        case BinOpTypeAssignTimes:
//        case BinOpTypeAssignTimesWrap:
//        case BinOpTypeAssignDiv:
//        case BinOpTypeAssignMod:
//        case BinOpTypeAssignPlus:
//        case BinOpTypeAssignPlusWrap:
//        case BinOpTypeAssignMinus:
//        case BinOpTypeAssignMinusWrap:
//        case BinOpTypeAssignBitShiftLeft:
//        case BinOpTypeAssignBitShiftLeftWrap:
//        case BinOpTypeAssignBitShiftRight:
//        case BinOpTypeAssignBitAnd:
//        case BinOpTypeAssignBitXor:
//        case BinOpTypeAssignBitOr:
//        case BinOpTypeAssignBoolAnd:
//        case BinOpTypeAssignBoolOr:
//            {
//                AstNode *lhs_node = node->data.bin_op_expr.op1;
//
//                TypeTableEntry *expected_rhs_type = analyze_lvalue(g, import, context, lhs_node,
//                        LValPurposeAssign, false);
//                if (expected_rhs_type->id == TypeTableEntryIdInvalid) {
//                    return g->builtin_types.entry_invalid;
//                } else if (!is_op_allowed(expected_rhs_type, node->data.bin_op_expr.bin_op)) {
//                    if (expected_rhs_type->id != TypeTableEntryIdInvalid) {
//                        add_node_error(g, lhs_node,
//                            buf_sprintf("operator not allowed for type '%s'",
//                                buf_ptr(&expected_rhs_type->name)));
//                    }
//                }
//
//                analyze_expression(g, import, context, expected_rhs_type, node->data.bin_op_expr.op2);
//                // not const ok because expression has side effects
//                return g->builtin_types.entry_void;
//            }
//        case BinOpTypeBoolOr:
//        case BinOpTypeBoolAnd:
//            return analyze_logic_bin_op_expr(g, import, context, node);
//        case BinOpTypeCmpEq:
//        case BinOpTypeCmpNotEq:
//        case BinOpTypeCmpLessThan:
//        case BinOpTypeCmpGreaterThan:
//        case BinOpTypeCmpLessOrEq:
//        case BinOpTypeCmpGreaterOrEq:
//            return analyze_bool_bin_op_expr(g, import, context, node);
//        case BinOpTypeBinOr:
//        case BinOpTypeBinXor:
//        case BinOpTypeBinAnd:
//        case BinOpTypeBitShiftLeft:
//        case BinOpTypeBitShiftLeftWrap:
//        case BinOpTypeBitShiftRight:
//        case BinOpTypeAdd:
//        case BinOpTypeAddWrap:
//        case BinOpTypeSub:
//        case BinOpTypeSubWrap:
//        case BinOpTypeMult:
//        case BinOpTypeMultWrap:
//        case BinOpTypeDiv:
//        case BinOpTypeMod:
//            {
//                AstNode **op1 = node->data.bin_op_expr.op1->parent_field;
//                AstNode **op2 = node->data.bin_op_expr.op2->parent_field;
//                TypeTableEntry *lhs_type = analyze_expression(g, import, context, nullptr, *op1);
//                TypeTableEntry *rhs_type = analyze_expression(g, import, context, nullptr, *op2);
//
//                AstNode *op_nodes[] = {*op1, *op2};
//                TypeTableEntry *op_types[] = {lhs_type, rhs_type};
//
//                TypeTableEntry *resolved_type = resolve_peer_type_compatibility(g, import, context, node,
//                        op_nodes, op_types, 2);
//
//                if (resolved_type->id == TypeTableEntryIdInvalid) {
//                    return resolved_type;
//                }
//
//                if (resolved_type->id == TypeTableEntryIdInt ||
//                    resolved_type->id == TypeTableEntryIdNumLitInt)
//                {
//                    // int
//                } else if ((resolved_type->id == TypeTableEntryIdFloat ||
//                           resolved_type->id == TypeTableEntryIdNumLitFloat) &&
//                    (bin_op_type == BinOpTypeAdd ||
//                     bin_op_type == BinOpTypeSub ||
//                     bin_op_type == BinOpTypeMult ||
//                     bin_op_type == BinOpTypeDiv ||
//                     bin_op_type == BinOpTypeMod))
//                {
//                    // float
//                } else {
//                    add_node_error(g, node, buf_sprintf("invalid operands to binary expression: '%s' and '%s'",
//                            buf_ptr(&lhs_type->name), buf_ptr(&rhs_type->name)));
//                    return g->builtin_types.entry_invalid;
//                }
//
//                ConstExprValue *op1_val = &get_resolved_expr(*op1)->const_val;
//                ConstExprValue *op2_val = &get_resolved_expr(*op2)->const_val;
//                if (!op1_val->ok || !op2_val->ok) {
//                    return resolved_type;
//                }
//
//                ConstExprValue *out_val = &get_resolved_expr(node)->const_val;
//                int err;
//                if ((err = eval_const_expr_bin_op(op1_val, resolved_type, bin_op_type,
//                                op2_val, resolved_type, out_val)))
//                {
//                    if (err == ErrorDivByZero) {
//                        add_node_error(g, node, buf_sprintf("division by zero is undefined"));
//                        return g->builtin_types.entry_invalid;
//                    } else if (err == ErrorOverflow) {
//                        add_node_error(g, node, buf_sprintf("value cannot be represented in any integer type"));
//                        return g->builtin_types.entry_invalid;
//                    }
//                    return g->builtin_types.entry_invalid;
//                }
//
//                num_lit_fits_in_other_type(g, node, resolved_type);
//                return resolved_type;
//            }
//        case BinOpTypeUnwrapMaybe:
//            {
//                AstNode *op1 = node->data.bin_op_expr.op1;
//                AstNode *op2 = node->data.bin_op_expr.op2;
//                TypeTableEntry *lhs_type = analyze_expression(g, import, context, nullptr, op1);
//
//                if (lhs_type->id == TypeTableEntryIdInvalid) {
//                    return lhs_type;
//                } else if (lhs_type->id == TypeTableEntryIdMaybe) {
//                    TypeTableEntry *child_type = lhs_type->data.maybe.child_type;
//                    analyze_expression(g, import, context, child_type, op2);
//                    return child_type;
//                } else {
//                    add_node_error(g, op1,
//                        buf_sprintf("expected maybe type, got '%s'",
//                            buf_ptr(&lhs_type->name)));
//                    return g->builtin_types.entry_invalid;
//                }
//            }
//        case BinOpTypeArrayCat:
//            {
//                AstNode **op1 = node->data.bin_op_expr.op1->parent_field;
//                AstNode **op2 = node->data.bin_op_expr.op2->parent_field;
//
//                TypeTableEntry *op1_type = analyze_expression(g, import, context, nullptr, *op1);
//                TypeTableEntry *child_type;
//                if (op1_type->id == TypeTableEntryIdInvalid) {
//                    return g->builtin_types.entry_invalid;
//                } else if (op1_type->id == TypeTableEntryIdArray) {
//                    child_type = op1_type->data.array.child_type;
//                } else if (op1_type->id == TypeTableEntryIdPointer &&
//                           op1_type->data.pointer.child_type == g->builtin_types.entry_u8) {
//                    child_type = op1_type->data.pointer.child_type;
//                } else {
//                    add_node_error(g, *op1, buf_sprintf("expected array or C string literal, got '%s'",
//                                buf_ptr(&op1_type->name)));
//                    return g->builtin_types.entry_invalid;
//                }
//
//                TypeTableEntry *op2_type = analyze_expression(g, import, context, nullptr, *op2);
//
//                if (op2_type->id == TypeTableEntryIdInvalid) {
//                    return g->builtin_types.entry_invalid;
//                } else if (op2_type->id == TypeTableEntryIdArray) {
//                    if (op2_type->data.array.child_type != child_type) {
//                        add_node_error(g, *op2, buf_sprintf("expected array of type '%s', got '%s'",
//                                    buf_ptr(&child_type->name),
//                                    buf_ptr(&op2_type->name)));
//                        return g->builtin_types.entry_invalid;
//                    }
//                } else if (op2_type->id == TypeTableEntryIdPointer &&
//                        op2_type->data.pointer.child_type == g->builtin_types.entry_u8) {
//                } else {
//                    add_node_error(g, *op2, buf_sprintf("expected array or C string literal, got '%s'",
//                                buf_ptr(&op2_type->name)));
//                    return g->builtin_types.entry_invalid;
//                }
//
//                ConstExprValue *op1_val = &get_resolved_expr(*op1)->const_val;
//                ConstExprValue *op2_val = &get_resolved_expr(*op2)->const_val;
//
//                AstNode *bad_node;
//                if (!op1_val->ok) {
//                    bad_node = *op1;
//                } else if (!op2_val->ok) {
//                    bad_node = *op2;
//                } else {
//                    bad_node = nullptr;
//                }
//                if (bad_node) {
//                    add_node_error(g, bad_node, buf_sprintf("array concatenation requires constant expression"));
//                    return g->builtin_types.entry_invalid;
//                }
//
//                ConstExprValue *const_val = &get_resolved_expr(node)->const_val;
//                const_val->ok = true;
//                const_val->depends_on_compile_var = op1_val->depends_on_compile_var ||
//                    op2_val->depends_on_compile_var;
//
//                if (op1_type->id == TypeTableEntryIdArray) {
//                    uint64_t new_len = op1_type->data.array.len + op2_type->data.array.len;
//                    const_val->data.x_array.fields = allocate<ConstExprValue*>(new_len);
//                    uint64_t next_index = 0;
//                    for (uint64_t i = 0; i < op1_type->data.array.len; i += 1, next_index += 1) {
//                        const_val->data.x_array.fields[next_index] = op1_val->data.x_array.fields[i];
//                    }
//                    for (uint64_t i = 0; i < op2_type->data.array.len; i += 1, next_index += 1) {
//                        const_val->data.x_array.fields[next_index] = op2_val->data.x_array.fields[i];
//                    }
//                    return get_array_type(g, child_type, new_len);
//                } else if (op1_type->id == TypeTableEntryIdPointer) {
//                    if (!op1_val->data.x_ptr.is_c_str) {
//                        add_node_error(g, *op1,
//                                buf_sprintf("expected array or C string literal, got '%s'",
//                                    buf_ptr(&op1_type->name)));
//                        return g->builtin_types.entry_invalid;
//                    } else if (!op2_val->data.x_ptr.is_c_str) {
//                        add_node_error(g, *op2,
//                                buf_sprintf("expected array or C string literal, got '%s'",
//                                    buf_ptr(&op2_type->name)));
//                        return g->builtin_types.entry_invalid;
//                    }
//                    const_val->data.x_ptr.is_c_str = true;
//                    const_val->data.x_ptr.len = op1_val->data.x_ptr.len + op2_val->data.x_ptr.len - 1;
//                    const_val->data.x_ptr.ptr = allocate<ConstExprValue*>(const_val->data.x_ptr.len);
//                    uint64_t next_index = 0;
//                    for (uint64_t i = 0; i < op1_val->data.x_ptr.len - 1; i += 1, next_index += 1) {
//                        const_val->data.x_ptr.ptr[next_index] = op1_val->data.x_ptr.ptr[i];
//                    }
//                    for (uint64_t i = 0; i < op2_val->data.x_ptr.len; i += 1, next_index += 1) {
//                        const_val->data.x_ptr.ptr[next_index] = op2_val->data.x_ptr.ptr[i];
//                    }
//                    return op1_type;
//                } else {
//                    zig_unreachable();
//                }
//            }
//        case BinOpTypeArrayMult:
//            return analyze_array_mult(g, import, context, expected_type, node);
//        case BinOpTypeInvalid:
//            zig_unreachable();
//    }
//    zig_unreachable();
//}


//static TypeTableEntry *analyze_bool_bin_op_expr(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        AstNode *node)
//{
//    assert(node->type == NodeTypeBinOpExpr);
//    BinOpType bin_op_type = node->data.bin_op_expr.bin_op;
//
//    AstNode **op1 = &node->data.bin_op_expr.op1;
//    AstNode **op2 = &node->data.bin_op_expr.op2;
//    TypeTableEntry *op1_type = analyze_expression(g, import, context, nullptr, *op1);
//    TypeTableEntry *op2_type = analyze_expression(g, import, context, nullptr, *op2);
//
//    AstNode *op_nodes[] = {*op1, *op2};
//    TypeTableEntry *op_types[] = {op1_type, op2_type};
//
//    TypeTableEntry *resolved_type = resolve_peer_type_compatibility(g, import, context, node,
//            op_nodes, op_types, 2);
//
//    bool is_equality_cmp = (bin_op_type == BinOpTypeCmpEq || bin_op_type == BinOpTypeCmpNotEq);
//
//    switch (resolved_type->id) {
//        case TypeTableEntryIdInvalid:
//            return g->builtin_types.entry_invalid;
//
//        case TypeTableEntryIdNumLitFloat:
//        case TypeTableEntryIdNumLitInt:
//        case TypeTableEntryIdInt:
//        case TypeTableEntryIdFloat:
//            break;
//
//        case TypeTableEntryIdBool:
//        case TypeTableEntryIdMetaType:
//        case TypeTableEntryIdVoid:
//        case TypeTableEntryIdPointer:
//        case TypeTableEntryIdPureError:
//        case TypeTableEntryIdFn:
//        case TypeTableEntryIdTypeDecl:
//        case TypeTableEntryIdNamespace:
//        case TypeTableEntryIdBlock:
//        case TypeTableEntryIdGenericFn:
//            if (!is_equality_cmp) {
//                add_node_error(g, node,
//                    buf_sprintf("operator not allowed for type '%s'", buf_ptr(&resolved_type->name)));
//                return g->builtin_types.entry_invalid;
//            }
//            break;
//
//        case TypeTableEntryIdEnum:
//            if (!is_equality_cmp || resolved_type->data.enumeration.gen_field_count != 0) {
//                add_node_error(g, node,
//                    buf_sprintf("operator not allowed for type '%s'", buf_ptr(&resolved_type->name)));
//                return g->builtin_types.entry_invalid;
//            }
//            break;
//
//        case TypeTableEntryIdUnreachable:
//        case TypeTableEntryIdArray:
//        case TypeTableEntryIdStruct:
//        case TypeTableEntryIdUndefLit:
//        case TypeTableEntryIdNullLit:
//        case TypeTableEntryIdMaybe:
//        case TypeTableEntryIdErrorUnion:
//        case TypeTableEntryIdUnion:
//            add_node_error(g, node,
//                buf_sprintf("operator not allowed for type '%s'", buf_ptr(&resolved_type->name)));
//            return g->builtin_types.entry_invalid;
//
//        case TypeTableEntryIdVar:
//            zig_unreachable();
//    }
//
//    ConstExprValue *op1_val = &get_resolved_expr(*op1)->const_val;
//    ConstExprValue *op2_val = &get_resolved_expr(*op2)->const_val;
//    if (!op1_val->ok || !op2_val->ok) {
//        return g->builtin_types.entry_bool;
//    }
//
//
//    ConstExprValue *out_val = &get_resolved_expr(node)->const_val;
//    eval_const_expr_bin_op(op1_val, op1_type, bin_op_type, op2_val, op2_type, out_val);
//    return g->builtin_types.entry_bool;
//
//}
//
////
//static TypeTableEntry *analyze_if(CodeGen *g, ImportTableEntry *import, BlockContext *parent_context,
//        TypeTableEntry *expected_type, AstNode *node,
//        AstNode **then_node, AstNode **else_node, bool cond_is_const, bool cond_bool_val)
//{
//    if (!*else_node) {
//        *else_node = create_ast_void_node(g, import, node);
//        normalize_parent_ptrs(node);
//    }
//
//    BlockContext *then_context;
//    BlockContext *else_context;
//    if (cond_is_const) {
//        if (cond_bool_val) {
//            then_context = parent_context;
//            else_context = new_block_context(node, parent_context);
//
//            else_context->codegen_excluded = true;
//        } else {
//            then_context = new_block_context(node, parent_context);
//            else_context = parent_context;
//
//            then_context->codegen_excluded = true;
//        }
//    } else {
//        then_context = parent_context;
//        else_context = parent_context;
//    }
//
//    TypeTableEntry *then_type = nullptr;
//    TypeTableEntry *else_type = nullptr;
//
//    if (!then_context->codegen_excluded) {
//        then_type = analyze_expression(g, import, then_context, expected_type, *then_node);
//        if (then_type->id == TypeTableEntryIdInvalid) {
//            return g->builtin_types.entry_invalid;
//        }
//    }
//    if (!else_context->codegen_excluded) {
//        else_type = analyze_expression(g, import, else_context, expected_type, *else_node);
//        if (else_type->id == TypeTableEntryIdInvalid) {
//            return g->builtin_types.entry_invalid;
//        }
//    }
//
//    TypeTableEntry *result_type;
//    if (then_context->codegen_excluded) {
//        result_type = else_type;
//    } else if (else_context->codegen_excluded) {
//        result_type = then_type;
//    } else if (expected_type) {
//        result_type = (then_type->id == TypeTableEntryIdUnreachable) ? else_type : then_type;
//    } else {
//        AstNode *op_nodes[] = {*then_node, *else_node};
//        TypeTableEntry *op_types[] = {then_type, else_type};
//        result_type = resolve_peer_type_compatibility(g, import, parent_context, node, op_nodes, op_types, 2);
//    }
//
//    if (!cond_is_const) {
//        return add_error_if_type_is_num_lit(g, result_type, node);
//    }
//
//    ConstExprValue *other_const_val;
//    if (cond_bool_val) {
//        other_const_val = &get_resolved_expr(*then_node)->const_val;
//    } else {
//        other_const_val = &get_resolved_expr(*else_node)->const_val;
//    }
//    if (!other_const_val->ok) {
//        return add_error_if_type_is_num_lit(g, result_type, node);
//    }
//
//    ConstExprValue *const_val = &get_resolved_expr(node)->const_val;
//    *const_val = *other_const_val;
//    // the condition depends on a compile var, so the entire if statement does too
//    const_val->depends_on_compile_var = true;
//    return result_type;
//}
//
//static TypeTableEntry *analyze_if_var_expr(CodeGen *g, ImportTableEntry *import, BlockContext *parent_context,
//        TypeTableEntry *expected_type, AstNode *node)
//{
//    assert(node->type == NodeTypeIfVarExpr);
//
//    BlockContext *child_context = new_block_context(node, parent_context);
//
//    analyze_variable_declaration_raw(g, import, child_context, node, &node->data.if_var_expr.var_decl, true,
//        nullptr, node->data.if_var_expr.var_is_ptr);
//    VariableTableEntry *var = node->data.if_var_expr.var_decl.variable;
//    if (var->type->id == TypeTableEntryIdInvalid) {
//        return g->builtin_types.entry_invalid;
//    }
//    AstNode *var_expr_node = node->data.if_var_expr.var_decl.expr;
//    ConstExprValue *var_const_val = &get_resolved_expr(var_expr_node)->const_val;
//    bool cond_is_const = var_const_val->ok;
//    bool cond_bool_val = cond_is_const ? (var_const_val->data.x_maybe != nullptr) : false;
//
//
//    AstNode **then_node = &node->data.if_var_expr.then_block;
//    AstNode **else_node = &node->data.if_var_expr.else_node;
//
//    return analyze_if(g, import, child_context, expected_type,
//            node, then_node, else_node, cond_is_const, cond_bool_val);
//}
//
//static TypeTableEntry *bad_method_call(CodeGen *g, AstNode *node, TypeTableEntry *container_type,
//        TypeTableEntry *expected_param_type, FnTableEntry *fn_table_entry)
//{
//    ErrorMsg *msg = add_node_error(g, node,
//        buf_sprintf("function called as method of '%s', but first parameter is of type '%s'",
//            buf_ptr(&container_type->name),
//            buf_ptr(&expected_param_type->name)));
//    if (fn_table_entry) {
//        add_error_note(g, msg, fn_table_entry->proto_node, buf_sprintf("function declared here"));
//    }
//    return g->builtin_types.entry_invalid;
//}
//
//// Before calling this function, set node->data.fn_call_expr.fn_table_entry if the function is known
//// at compile time. Otherwise this is a function pointer call.
//static TypeTableEntry *analyze_fn_call_ptr(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        TypeTableEntry *expected_type, AstNode *node, TypeTableEntry *fn_type,
//        AstNode *struct_node)
//{
//    assert(node->type == NodeTypeFnCallExpr);
//
//    if (fn_type->id == TypeTableEntryIdInvalid) {
//        return fn_type;
//    }
//
//    // The function call might include inline parameters which we need to ignore according to the
//    // fn_type.
//    FnTableEntry *fn_table_entry = node->data.fn_call_expr.fn_entry;
//    AstNode *generic_proto_node = fn_table_entry ?
//        fn_table_entry->proto_node->data.fn_proto.generic_proto_node : nullptr;
//
//    // count parameters
//    size_t struct_node_1_or_0 = struct_node ? 1 : 0;
//    size_t src_param_count = fn_type->data.fn.fn_type_id.param_count +
//        (generic_proto_node ? generic_proto_node->data.fn_proto.inline_arg_count : 0);
//    size_t call_param_count = node->data.fn_call_expr.params.length;
//    size_t expect_arg_count = src_param_count - struct_node_1_or_0;
//
//    bool ok_invocation = true;
//
//    if (fn_type->data.fn.fn_type_id.is_var_args) {
//        if (call_param_count < expect_arg_count) {
//            ok_invocation = false;
//            add_node_error(g, node,
//                buf_sprintf("expected at least %zu arguments, got %zu", src_param_count, call_param_count));
//        }
//    } else if (expect_arg_count != call_param_count) {
//        ok_invocation = false;
//        add_node_error(g, node,
//                buf_sprintf("expected %zu arguments, got %zu", expect_arg_count, call_param_count));
//    }
//
//    bool all_args_const_expr = true;
//
//    if (struct_node) {
//        Expr *struct_expr = get_resolved_expr(struct_node);
//        ConstExprValue *struct_const_val = &struct_expr->const_val;
//        if (!struct_const_val->ok) {
//            all_args_const_expr = false;
//        }
//
//        FnTypeParamInfo *param_info = &fn_type->data.fn.fn_type_id.param_info[0];
//        TypeTableEntry *expected_param_type = param_info->type;
//        TypeTableEntry *container_bare_type = container_ref_type(struct_expr->type_entry);
//        if (is_container_ref(expected_param_type)) {
//            TypeTableEntry *param_bare_type = container_ref_type(expected_param_type);
//            if (param_bare_type != container_bare_type) {
//                return bad_method_call(g, node, container_bare_type, expected_param_type, fn_table_entry);
//            }
//        } else {
//            return bad_method_call(g, node, container_bare_type, expected_param_type, fn_table_entry);
//        }
//    }
//
//    // analyze each parameter. in the case of a method, we already analyzed the
//    // first parameter in order to figure out which struct we were calling a method on.
//    size_t next_type_i = struct_node_1_or_0;
//    for (size_t call_i = 0; call_i < call_param_count; call_i += 1) {
//        size_t proto_i = call_i + struct_node_1_or_0;
//        AstNode **param_node = &node->data.fn_call_expr.params.at(call_i);
//        // determine the expected type for each parameter
//        TypeTableEntry *expected_param_type = nullptr;
//        if (proto_i < src_param_count) {
//            if (generic_proto_node &&
//                generic_proto_node->data.fn_proto.params.at(proto_i)->data.param_decl.is_inline)
//            {
//                continue;
//            }
//
//            FnTypeParamInfo *param_info = &fn_type->data.fn.fn_type_id.param_info[next_type_i];
//            next_type_i += 1;
//
//            expected_param_type = param_info->type;
//        }
//        TypeTableEntry *param_type = analyze_expression(g, import, context, expected_param_type, *param_node);
//        if (param_type->id == TypeTableEntryIdInvalid) {
//            return param_type;
//        }
//
//        ConstExprValue *const_arg_val = &get_resolved_expr(*param_node)->const_val;
//        if (!const_arg_val->ok) {
//            all_args_const_expr = false;
//        }
//    }
//
//    TypeTableEntry *return_type = fn_type->data.fn.fn_type_id.return_type;
//
//    if (return_type->id == TypeTableEntryIdInvalid) {
//        return return_type;
//    }
//
//    ConstExprValue *result_val = &get_resolved_expr(node)->const_val;
//    if (ok_invocation && fn_table_entry && fn_table_entry->is_pure && fn_table_entry->want_pure != WantPureFalse) {
//        if (fn_table_entry->anal_state == FnAnalStateReady) {
//            analyze_fn_body(g, fn_table_entry);
//            if (fn_table_entry->proto_node->data.fn_proto.skip) {
//                return g->builtin_types.entry_invalid;
//            }
//        }
//        if (all_args_const_expr) {
//            if (fn_table_entry->is_pure && fn_table_entry->anal_state == FnAnalStateComplete) {
//                if (eval_fn(g, node, fn_table_entry, result_val, 1000, struct_node)) {
//                    // function evaluation generated an error
//                    return g->builtin_types.entry_invalid;
//                }
//                return return_type;
//            }
//        }
//    }
//    if (!ok_invocation || !fn_table_entry || !fn_table_entry->is_pure || fn_table_entry->want_pure == WantPureFalse) {
//        // calling an impure fn is impure
//        mark_impure_fn(g, context, node);
//        if (fn_table_entry && fn_table_entry->want_pure == WantPureTrue) {
//            return g->builtin_types.entry_invalid;
//        }
//    }
//
//    // TODO
//    //if (handle_is_ptr(return_type)) {
//    //    if (context->fn_entry) {
//    //        context->fn_entry->cast_alloca_list.append(node);
//    //    } else if (!result_val->ok) {
//    //        add_node_error(g, node, buf_sprintf("unable to evaluate constant expression"));
//    //    }
//    //}
//
//    return return_type;
//}
//
//static TypeTableEntry *analyze_fn_call_with_inline_args(CodeGen *g, ImportTableEntry *import,
//        BlockContext *parent_context, TypeTableEntry *expected_type, AstNode *call_node,
//        FnTableEntry *fn_table_entry, AstNode *struct_node)
//{
//    assert(call_node->type == NodeTypeFnCallExpr);
//    assert(fn_table_entry);
//
//    AstNode *decl_node = fn_table_entry->proto_node;
//
//    // count parameters
//    size_t struct_node_1_or_0 = (struct_node ? 1 : 0);
//    size_t src_param_count = decl_node->data.fn_proto.params.length;
//    size_t call_param_count = call_node->data.fn_call_expr.params.length;
//
//    if (src_param_count != call_param_count + struct_node_1_or_0) {
//        add_node_error(g, call_node,
//            buf_sprintf("expected %zu arguments, got %zu", src_param_count - struct_node_1_or_0, call_param_count));
//        return g->builtin_types.entry_invalid;
//    }
//
//    size_t inline_or_var_type_arg_count = decl_node->data.fn_proto.inline_or_var_type_arg_count;
//    assert(inline_or_var_type_arg_count > 0);
//
//    BlockContext *child_context = decl_node->owner->block_context;
//    size_t next_generic_param_index = 0;
//
//    GenericFnTypeId *generic_fn_type_id = allocate<GenericFnTypeId>(1);
//    generic_fn_type_id->decl_node = decl_node;
//    generic_fn_type_id->generic_param_count = inline_or_var_type_arg_count;
//    generic_fn_type_id->generic_params = allocate<GenericParamValue>(inline_or_var_type_arg_count);
//
//    size_t next_impl_i = 0;
//    for (size_t call_i = 0; call_i < call_param_count; call_i += 1) {
//        size_t proto_i = call_i + struct_node_1_or_0;
//        AstNode *generic_param_decl_node = decl_node->data.fn_proto.params.at(proto_i);
//        assert(generic_param_decl_node->type == NodeTypeParamDecl);
//
//        AstNode **generic_param_type_node = &generic_param_decl_node->data.param_decl.type;
//        TypeTableEntry *expected_param_type = analyze_type_expr(g, decl_node->owner, child_context,
//                *generic_param_type_node);
//        if (expected_param_type->id == TypeTableEntryIdInvalid) {
//            return expected_param_type;
//        }
//
//        bool is_var_type = (expected_param_type->id == TypeTableEntryIdVar);
//        bool is_inline = generic_param_decl_node->data.param_decl.is_inline;
//        if (!is_inline && !is_var_type) {
//            next_impl_i += 1;
//            continue;
//        }
//
//
//        AstNode **param_node = &call_node->data.fn_call_expr.params.at(call_i);
//        TypeTableEntry *param_type = analyze_expression(g, import, parent_context,
//                is_var_type ? nullptr : expected_param_type, *param_node);
//        if (param_type->id == TypeTableEntryIdInvalid) {
//            return param_type;
//        }
//
//        // set child_context so that the previous param is in scope
//        child_context = new_block_context(generic_param_decl_node, child_context);
//
//        ConstExprValue *const_val = &get_resolved_expr(*param_node)->const_val;
//        if (is_inline && !const_val->ok) {
//            add_node_error(g, *param_node,
//                    buf_sprintf("unable to evaluate constant expression for inline parameter"));
//
//            return g->builtin_types.entry_invalid;
//        }
//
//        VariableTableEntry *var = add_local_var_shadowable(g, generic_param_decl_node, decl_node->owner, child_context,
//                generic_param_decl_node->data.param_decl.name, param_type, true, *param_node, true);
//        // This generic function instance could be called with anything, so when this variable is read it
//        // needs to know that it depends on compile time variable data.
//        var->force_depends_on_compile_var = true;
//
//        GenericParamValue *generic_param_value =
//            &generic_fn_type_id->generic_params[next_generic_param_index];
//        generic_param_value->type = param_type;
//        generic_param_value->node = is_inline ? *param_node : nullptr;
//        generic_param_value->impl_index = next_impl_i;
//        next_generic_param_index += 1;
//
//        if (!is_inline) {
//            next_impl_i += 1;
//        }
//    }
//
//    assert(next_generic_param_index == inline_or_var_type_arg_count);
//
//    auto entry = g->generic_table.maybe_get(generic_fn_type_id);
//    FnTableEntry *impl_fn;
//    if (entry) {
//        AstNode *impl_decl_node = entry->value;
//        assert(impl_decl_node->type == NodeTypeFnProto);
//        impl_fn = impl_decl_node->data.fn_proto.fn_table_entry;
//    } else {
//        AstNode *decl_node = generic_fn_type_id->decl_node;
//        AstNode *impl_fn_def_node = ast_clone_subtree_special(decl_node->data.fn_proto.fn_def_node,
//                &g->next_node_index, AstCloneSpecialOmitInlineParams);
//        AstNode *impl_decl_node = impl_fn_def_node->data.fn_def.fn_proto;
//        impl_decl_node->data.fn_proto.inline_arg_count = 0;
//        impl_decl_node->data.fn_proto.inline_or_var_type_arg_count = 0;
//        impl_decl_node->data.fn_proto.generic_proto_node = decl_node;
//
//        // replace var arg types with actual types
//        for (size_t generic_arg_i = 0; generic_arg_i < inline_or_var_type_arg_count; generic_arg_i += 1) {
//            GenericParamValue *generic_param_value = &generic_fn_type_id->generic_params[generic_arg_i];
//            if (!generic_param_value->node) {
//                size_t impl_i = generic_param_value->impl_index;
//                AstNode *impl_param_decl_node = impl_decl_node->data.fn_proto.params.at(impl_i);
//                assert(impl_param_decl_node->type == NodeTypeParamDecl);
//
//                impl_param_decl_node->data.param_decl.type = create_ast_type_node(g, import,
//                        generic_param_value->type, impl_param_decl_node);
//                normalize_parent_ptrs(impl_param_decl_node);
//            }
//        }
//
//        preview_fn_proto_instance(g, import, impl_decl_node, child_context);
//        g->generic_table.put(generic_fn_type_id, impl_decl_node);
//        impl_fn = impl_decl_node->data.fn_proto.fn_table_entry;
//    }
//
//    call_node->data.fn_call_expr.fn_entry = impl_fn;
//    return analyze_fn_call_ptr(g, import, parent_context, expected_type, call_node,
//            impl_fn->type_entry, struct_node);
//}
//
//static TypeTableEntry *analyze_generic_fn_call(CodeGen *g, ImportTableEntry *import, BlockContext *parent_context,
//        TypeTableEntry *expected_type, AstNode *node, TypeTableEntry *generic_fn_type)
//{
//    assert(node->type == NodeTypeFnCallExpr);
//    assert(generic_fn_type->id == TypeTableEntryIdGenericFn);
//
//    AstNode *decl_node = generic_fn_type->data.generic_fn.decl_node;
//    assert(decl_node->type == NodeTypeContainerDecl);
//    ZigList<AstNode *> *generic_params = &decl_node->data.struct_decl.generic_params;
//
//    size_t expected_param_count = generic_params->length;
//    size_t actual_param_count = node->data.fn_call_expr.params.length;
//
//    if (actual_param_count != expected_param_count) {
//        add_node_error(g, first_executing_node(node),
//                buf_sprintf("expected %zu arguments, got %zu", expected_param_count, actual_param_count));
//        return g->builtin_types.entry_invalid;
//    }
//
//    GenericFnTypeId *generic_fn_type_id = allocate<GenericFnTypeId>(1);
//    generic_fn_type_id->decl_node = decl_node;
//    generic_fn_type_id->generic_param_count = actual_param_count;
//    generic_fn_type_id->generic_params = allocate<GenericParamValue>(actual_param_count);
//
//    BlockContext *child_context = decl_node->owner->block_context;
//    for (size_t i = 0; i < actual_param_count; i += 1) {
//        AstNode *generic_param_decl_node = generic_params->at(i);
//        assert(generic_param_decl_node->type == NodeTypeParamDecl);
//
//        AstNode **generic_param_type_node = &generic_param_decl_node->data.param_decl.type;
//
//        TypeTableEntry *expected_param_type = analyze_type_expr(g, decl_node->owner,
//                child_context, *generic_param_type_node);
//        if (expected_param_type->id == TypeTableEntryIdInvalid) {
//            return expected_param_type;
//        }
//
//
//
//        AstNode **param_node = &node->data.fn_call_expr.params.at(i);
//
//        TypeTableEntry *param_type = analyze_expression(g, import, parent_context, expected_param_type,
//                *param_node);
//        if (param_type->id == TypeTableEntryIdInvalid) {
//            return param_type;
//        }
//
//        // set child_context so that the previous param is in scope
//        child_context = new_block_context(generic_param_decl_node, child_context);
//
//        ConstExprValue *const_val = &get_resolved_expr(*param_node)->const_val;
//        if (const_val->ok) {
//            VariableTableEntry *var = add_local_var(g, generic_param_decl_node, decl_node->owner, child_context,
//                    generic_param_decl_node->data.param_decl.name, param_type, true, *param_node);
//            var->force_depends_on_compile_var = true;
//        } else {
//            add_node_error(g, *param_node, buf_sprintf("unable to evaluate constant expression"));
//
//            return g->builtin_types.entry_invalid;
//        }
//
//        GenericParamValue *generic_param_value = &generic_fn_type_id->generic_params[i];
//        generic_param_value->type = param_type;
//        generic_param_value->node = *param_node;
//    }
//
//    auto entry = g->generic_table.maybe_get(generic_fn_type_id);
//    if (entry) {
//        AstNode *impl_decl_node = entry->value;
//        assert(impl_decl_node->type == NodeTypeContainerDecl);
//        TypeTableEntry *type_entry = impl_decl_node->data.struct_decl.type_entry;
//        return resolve_expr_const_val_as_type(g, node, type_entry, false);
//    }
//
//    // make a type from the generic parameters supplied
//    assert(decl_node->type == NodeTypeContainerDecl);
//    AstNode *impl_decl_node = ast_clone_subtree(decl_node, &g->next_node_index);
//    g->generic_table.put(generic_fn_type_id, impl_decl_node);
//    scan_struct_decl(g, import, child_context, impl_decl_node);
//    TypeTableEntry *type_entry = impl_decl_node->data.struct_decl.type_entry;
//    resolve_struct_type(g, import, type_entry);
//    return resolve_expr_const_val_as_type(g, node, type_entry, false);
//}
//
//static TypeTableEntry *analyze_fn_call_expr(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        TypeTableEntry *expected_type, AstNode *node)
//{
//    AstNode *fn_ref_expr = node->data.fn_call_expr.fn_ref_expr;
//
//    if (node->data.fn_call_expr.is_builtin) {
//        zig_panic("moved builtin fn call code to ir.cpp");
//    }
//
//    TypeTableEntry *invoke_type_entry = analyze_expression(g, import, context, nullptr, fn_ref_expr);
//    if (invoke_type_entry->id == TypeTableEntryIdInvalid) {
//        return g->builtin_types.entry_invalid;
//    }
//
//    // use constant expression evaluator to figure out the function at compile time.
//    // otherwise we treat this as a function pointer.
//    ConstExprValue *const_val = &get_resolved_expr(fn_ref_expr)->const_val;
//
//    if (const_val->ok) {
//        if (invoke_type_entry->id == TypeTableEntryIdMetaType) {
//            zig_unreachable();
//        } else if (invoke_type_entry->id == TypeTableEntryIdFn) {
//            AstNode *struct_node;
//            if (fn_ref_expr->type == NodeTypeFieldAccessExpr &&
//                fn_ref_expr->data.field_access_expr.is_member_fn)
//            {
//                struct_node = fn_ref_expr->data.field_access_expr.struct_expr;
//            } else {
//                struct_node = nullptr;
//            }
//
//            FnTableEntry *fn_table_entry = const_val->data.x_fn;
//            node->data.fn_call_expr.fn_entry = fn_table_entry;
//            return analyze_fn_call_ptr(g, import, context, expected_type, node,
//                    fn_table_entry->type_entry, struct_node);
//        } else if (invoke_type_entry->id == TypeTableEntryIdGenericFn) {
//            TypeTableEntry *generic_fn_type = const_val->data.x_type;
//            AstNode *decl_node = generic_fn_type->data.generic_fn.decl_node;
//            if (decl_node->type == NodeTypeFnProto) {
//                AstNode *struct_node;
//                if (fn_ref_expr->type == NodeTypeFieldAccessExpr &&
//                    fn_ref_expr->data.field_access_expr.is_member_fn)
//                {
//                    struct_node = fn_ref_expr->data.field_access_expr.struct_expr;
//                } else {
//                    struct_node = nullptr;
//                }
//
//                FnTableEntry *fn_table_entry = decl_node->data.fn_proto.fn_table_entry;
//                if (fn_table_entry->proto_node->data.fn_proto.skip) {
//                    return g->builtin_types.entry_invalid;
//                }
//                return analyze_fn_call_with_inline_args(g, import, context, expected_type, node,
//                        fn_table_entry, struct_node);
//            } else {
//                return analyze_generic_fn_call(g, import, context, expected_type, node, const_val->data.x_type);
//            }
//        } else {
//            add_node_error(g, fn_ref_expr,
//                buf_sprintf("type '%s' not a function", buf_ptr(&invoke_type_entry->name)));
//            return g->builtin_types.entry_invalid;
//        }
//    }
//
//    // function pointer
//    if (invoke_type_entry->id == TypeTableEntryIdFn) {
//        return analyze_fn_call_ptr(g, import, context, expected_type, node, invoke_type_entry, nullptr);
//    } else {
//        add_node_error(g, fn_ref_expr,
//            buf_sprintf("type '%s' not a function", buf_ptr(&invoke_type_entry->name)));
//        return g->builtin_types.entry_invalid;
//    }
//}
//static TypeTableEntry *analyze_switch_expr(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        TypeTableEntry *expected_type, AstNode *node)
//{
//    AstNode **expr_node = &node->data.switch_expr.expr;
//    TypeTableEntry *expr_type = analyze_expression(g, import, context, nullptr, *expr_node);
//    ConstExprValue *expr_val = &get_resolved_expr(*expr_node)->const_val;
//    if (expr_val->ok && !expr_val->depends_on_compile_var) {
//        add_node_error(g, first_executing_node(*expr_node),
//                buf_sprintf("value is constant; unnecessary switch statement"));
//    }
//    ConstExprValue *const_val = &get_resolved_expr(node)->const_val;
//
//
//    size_t prong_count = node->data.switch_expr.prongs.length;
//    AstNode **peer_nodes = allocate<AstNode*>(prong_count);
//    TypeTableEntry **peer_types = allocate<TypeTableEntry*>(prong_count);
//
//    bool any_errors = false;
//    if (expr_type->id == TypeTableEntryIdInvalid) {
//        return expr_type;
//    } else if (expr_type->id == TypeTableEntryIdUnreachable) {
//        add_node_error(g, first_executing_node(*expr_node),
//                buf_sprintf("switch on unreachable expression not allowed"));
//        return g->builtin_types.entry_invalid;
//    }
//
//
//    size_t *field_use_counts = nullptr;
//    HashMap<int, AstNode *, int_hash, int_eq> err_use_nodes = {};
//    if (expr_type->id == TypeTableEntryIdEnum) {
//        field_use_counts = allocate<size_t>(expr_type->data.enumeration.src_field_count);
//    } else if (expr_type->id == TypeTableEntryIdErrorUnion) {
//        err_use_nodes.init(10);
//    }
//
//    size_t *const_chosen_prong_index = &node->data.switch_expr.const_chosen_prong_index;
//    *const_chosen_prong_index = SIZE_MAX;
//    AstNode *else_prong = nullptr;
//    for (size_t prong_i = 0; prong_i < prong_count; prong_i += 1) {
//        AstNode *prong_node = node->data.switch_expr.prongs.at(prong_i);
//
//        TypeTableEntry *var_type;
//        bool var_is_target_expr;
//        if (prong_node->data.switch_prong.items.length == 0) {
//            if (else_prong) {
//                add_node_error(g, prong_node, buf_sprintf("multiple else prongs in switch expression"));
//                any_errors = true;
//            } else {
//                else_prong = prong_node;
//            }
//            var_type = expr_type;
//            var_is_target_expr = true;
//            if (*const_chosen_prong_index == SIZE_MAX && expr_val->ok) {
//                *const_chosen_prong_index = prong_i;
//            }
//        } else {
//            bool all_agree_on_var_type = true;
//            var_type = nullptr;
//
//            for (size_t item_i = 0; item_i < prong_node->data.switch_prong.items.length; item_i += 1) {
//                AstNode *item_node = prong_node->data.switch_prong.items.at(item_i);
//                if (item_node->type == NodeTypeSwitchRange) {
//                    zig_panic("TODO range in switch statement");
//                }
//
//                if (expr_type->id == TypeTableEntryIdEnum) {
//                    if (item_node->type == NodeTypeSymbol) {
//                        Buf *field_name = item_node->data.symbol_expr.symbol;
//                        TypeEnumField *type_enum_field = find_enum_type_field(expr_type, field_name);
//                        if (type_enum_field) {
//                            item_node->data.symbol_expr.enum_field = type_enum_field;
//                            if (!var_type) {
//                                var_type = type_enum_field->type_entry;
//                            }
//                            if (type_enum_field->type_entry != var_type) {
//                                all_agree_on_var_type = false;
//                            }
//                            uint32_t field_index = type_enum_field->value;
//                            assert(field_use_counts);
//                            field_use_counts[field_index] += 1;
//                            if (field_use_counts[field_index] > 1) {
//                                add_node_error(g, item_node,
//                                    buf_sprintf("duplicate switch value: '%s'",
//                                        buf_ptr(type_enum_field->name)));
//                                any_errors = true;
//                            }
//                            if (!any_errors && expr_val->ok) {
//                                if (expr_val->data.x_enum.tag == type_enum_field->value) {
//                                    *const_chosen_prong_index = prong_i;
//                                }
//                            }
//                        } else {
//                            add_node_error(g, item_node,
//                                    buf_sprintf("enum '%s' has no field '%s'",
//                                        buf_ptr(&expr_type->name), buf_ptr(field_name)));
//                            any_errors = true;
//                        }
//                    } else {
//                        add_node_error(g, item_node, buf_sprintf("expected enum tag name"));
//                        any_errors = true;
//                    }
//                } else if (expr_type->id == TypeTableEntryIdErrorUnion) {
//                    if (item_node->type == NodeTypeSymbol) {
//                        Buf *err_name = item_node->data.symbol_expr.symbol;
//                        bool is_ok_case = buf_eql_str(err_name, "Ok");
//                        auto err_table_entry = is_ok_case ? nullptr: g->error_table.maybe_get(err_name);
//                        if (is_ok_case || err_table_entry) {
//                            uint32_t err_value = is_ok_case ? 0 : err_table_entry->value->value;
//                            item_node->data.symbol_expr.err_value = err_value;
//                            TypeTableEntry *this_var_type;
//                            if (is_ok_case) {
//                                this_var_type = expr_type->data.error.child_type;
//                            } else {
//                                this_var_type = g->builtin_types.entry_pure_error;
//                            }
//                            if (!var_type) {
//                                var_type = this_var_type;
//                            }
//                            if (this_var_type != var_type) {
//                                all_agree_on_var_type = false;
//                            }
//
//                            // detect duplicate switch values
//                            auto existing_entry = err_use_nodes.maybe_get(err_value);
//                            if (existing_entry) {
//                                add_node_error(g, existing_entry->value,
//                                        buf_sprintf("duplicate switch value: '%s'", buf_ptr(err_name)));
//                                any_errors = true;
//                            } else {
//                                err_use_nodes.put(err_value, item_node);
//                            }
//
//                            if (!any_errors && expr_val->ok) {
//                                if (expr_val->data.x_err.err->value == err_value) {
//                                    *const_chosen_prong_index = prong_i;
//                                }
//                            }
//                        } else {
//                            add_node_error(g, item_node,
//                                    buf_sprintf("use of undeclared error value '%s'", buf_ptr(err_name)));
//                            any_errors = true;
//                        }
//                    } else {
//                        add_node_error(g, item_node, buf_sprintf("expected error value name"));
//                        any_errors = true;
//                    }
//                } else {
//                    if (!any_errors && expr_val->ok) {
//                        // note: there is now a function in eval.cpp for doing const expr comparison
//                        zig_panic("TODO determine if const exprs are equal");
//                    }
//                    TypeTableEntry *item_type = analyze_expression(g, import, context, expr_type, item_node);
//                    if (item_type->id != TypeTableEntryIdInvalid) {
//                        ConstExprValue *const_val = &get_resolved_expr(item_node)->const_val;
//                        if (!const_val->ok) {
//                            add_node_error(g, item_node,
//                                buf_sprintf("unable to evaluate constant expression"));
//                            any_errors = true;
//                        }
//                    }
//                }
//            }
//            if (!var_type || !all_agree_on_var_type) {
//                var_type = expr_type;
//                var_is_target_expr = true;
//            } else {
//                var_is_target_expr = false;
//            }
//        }
//
//        BlockContext *child_context = new_block_context(node, context);
//        prong_node->data.switch_prong.block_context = child_context;
//        AstNode *var_node = prong_node->data.switch_prong.var_symbol;
//        if (var_node) {
//            assert(var_node->type == NodeTypeSymbol);
//            Buf *var_name = var_node->data.symbol_expr.symbol;
//            var_node->block_context = child_context;
//            prong_node->data.switch_prong.var = add_local_var(g, var_node, import,
//                    child_context, var_name, var_type, true, nullptr);
//            prong_node->data.switch_prong.var_is_target_expr = var_is_target_expr;
//        }
//    }
//
//    for (size_t prong_i = 0; prong_i < prong_count; prong_i += 1) {
//        AstNode *prong_node = node->data.switch_expr.prongs.at(prong_i);
//        BlockContext *child_context = prong_node->data.switch_prong.block_context;
//        child_context->codegen_excluded = expr_val->ok && (*const_chosen_prong_index != prong_i);
//
//        if (child_context->codegen_excluded) {
//            peer_types[prong_i] = g->builtin_types.entry_unreachable;
//        } else {
//            peer_types[prong_i] = analyze_expression(g, import, child_context, expected_type,
//                    prong_node->data.switch_prong.expr);
//        }
//        // This must go after the analyze_expression for
//        // prong_node->data.switch_prong.expr because of AST rewriting.
//        peer_nodes[prong_i] = prong_node->data.switch_prong.expr;
//    }
//
//    if (expr_type->id == TypeTableEntryIdEnum && !else_prong) {
//        for (uint32_t i = 0; i < expr_type->data.enumeration.src_field_count; i += 1) {
//            if (field_use_counts[i] == 0) {
//                add_node_error(g, node,
//                    buf_sprintf("enumeration value '%s' not handled in switch",
//                        buf_ptr(expr_type->data.enumeration.fields[i].name)));
//                any_errors = true;
//            }
//        }
//    }
//
//    if (any_errors) {
//        return g->builtin_types.entry_invalid;
//    }
//
//    if (prong_count == 0) {
//        add_node_error(g, node, buf_sprintf("switch statement has no prongs"));
//        return g->builtin_types.entry_invalid;
//    }
//
//    TypeTableEntry *result_type = resolve_peer_type_compatibility(g, import, context, node,
//            peer_nodes, peer_types, prong_count);
//
//    if (expr_val->ok) {
//        assert(*const_chosen_prong_index != SIZE_MAX);
//
//        *const_val = get_resolved_expr(peer_nodes[*const_chosen_prong_index])->const_val;
//        // the target expr depends on a compile var because we have an error on unnecessary
//        // switch statement, so the entire switch statement does too
//        const_val->depends_on_compile_var = true;
//
//        if (!const_val->ok) {
//            return add_error_if_type_is_num_lit(g, result_type, node);
//        }
//    } else {
//        return add_error_if_type_is_num_lit(g, result_type, node);
//    }
//
//    return result_type;
//}
//
//static TypeTableEntry *analyze_return_expr(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        TypeTableEntry *expected_type, AstNode *node)
//{
//    if (!node->data.return_expr.expr) {
//        node->data.return_expr.expr = create_ast_void_node(g, import, node);
//        normalize_parent_ptrs(node);
//    }
//
//    TypeTableEntry *expected_return_type = get_return_type(context);
//
//    switch (node->data.return_expr.kind) {
//        case ReturnKindUnconditional:
//            zig_panic("TODO moved to ir.cpp");
//        case ReturnKindError:
//            {
//                TypeTableEntry *expected_err_type;
//                if (expected_type) {
//                    expected_err_type = get_error_type(g, expected_type);
//                } else {
//                    expected_err_type = nullptr;
//                }
//                TypeTableEntry *resolved_type = analyze_expression(g, import, context, expected_err_type,
//                        node->data.return_expr.expr);
//                if (resolved_type->id == TypeTableEntryIdInvalid) {
//                    return resolved_type;
//                } else if (resolved_type->id == TypeTableEntryIdErrorUnion) {
//                    if (expected_return_type->id != TypeTableEntryIdErrorUnion &&
//                        expected_return_type->id != TypeTableEntryIdPureError)
//                    {
//                        ErrorMsg *msg = add_node_error(g, node,
//                            buf_sprintf("%%return statement in function with return type '%s'",
//                                buf_ptr(&expected_return_type->name)));
//                        AstNode *return_type_node = context->fn_entry->fn_def_node->data.fn_def.fn_proto->data.fn_proto.return_type;
//                        add_error_note(g, msg, return_type_node, buf_sprintf("function return type here"));
//                    }
//
//                    return resolved_type->data.error.child_type;
//                } else {
//                    add_node_error(g, node->data.return_expr.expr,
//                        buf_sprintf("expected error type, got '%s'", buf_ptr(&resolved_type->name)));
//                    return g->builtin_types.entry_invalid;
//                }
//            }
//        case ReturnKindMaybe:
//            {
//                TypeTableEntry *expected_maybe_type;
//                if (expected_type) {
//                    expected_maybe_type = get_maybe_type(g, expected_type);
//                } else {
//                    expected_maybe_type = nullptr;
//                }
//                TypeTableEntry *resolved_type = analyze_expression(g, import, context, expected_maybe_type,
//                        node->data.return_expr.expr);
//                if (resolved_type->id == TypeTableEntryIdInvalid) {
//                    return resolved_type;
//                } else if (resolved_type->id == TypeTableEntryIdMaybe) {
//                    if (expected_return_type->id != TypeTableEntryIdMaybe) {
//                        ErrorMsg *msg = add_node_error(g, node,
//                            buf_sprintf("?return statement in function with return type '%s'",
//                                buf_ptr(&expected_return_type->name)));
//                        AstNode *return_type_node = context->fn_entry->fn_def_node->data.fn_def.fn_proto->data.fn_proto.return_type;
//                        add_error_note(g, msg, return_type_node, buf_sprintf("function return type here"));
//                    }
//
//                    return resolved_type->data.maybe.child_type;
//                } else {
//                    add_node_error(g, node->data.return_expr.expr,
//                        buf_sprintf("expected maybe type, got '%s'", buf_ptr(&resolved_type->name)));
//                    return g->builtin_types.entry_invalid;
//                }
//            }
//    }
//    zig_unreachable();
//}
//static TypeTableEntry *analyze_goto_pass1(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        TypeTableEntry *expected_type, AstNode *node)
//{
//    assert(node->type == NodeTypeGoto);
//
//    FnTableEntry *fn_table_entry = context->fn_entry;
//    assert(fn_table_entry);
//
//    fn_table_entry->goto_list.append(node);
//
//    return g->builtin_types.entry_unreachable;
//}
//
//static TypeTableEntry *analyze_enum_value_expr(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        AstNode *field_access_node, AstNode *value_node, TypeTableEntry *enum_type, Buf *field_name,
//        AstNode *out_node)
//{
//    assert(field_access_node->type == NodeTypeFieldAccessExpr);
//
//    TypeEnumField *type_enum_field = find_enum_type_field(enum_type, field_name);
//    if (type_enum_field->type_entry->id == TypeTableEntryIdInvalid) {
//        return g->builtin_types.entry_invalid;
//    }
//
//    field_access_node->data.field_access_expr.type_enum_field = type_enum_field;
//
//    if (type_enum_field) {
//        if (value_node) {
//            AstNode **value_node_ptr = value_node->parent_field;
//            TypeTableEntry *value_type = analyze_expression(g, import, context,
//                    type_enum_field->type_entry, value_node);
//
//            if (value_type->id == TypeTableEntryIdInvalid) {
//                return g->builtin_types.entry_invalid;
//            }
//
//            StructValExprCodeGen *codegen = &field_access_node->data.field_access_expr.resolved_struct_val_expr;
//            codegen->type_entry = enum_type;
//            codegen->source_node = field_access_node;
//
//            ConstExprValue *value_const_val = &get_resolved_expr(*value_node_ptr)->const_val;
//            if (value_const_val->ok) {
//                ConstExprValue *const_val = &get_resolved_expr(out_node)->const_val;
//                const_val->ok = true;
//                const_val->data.x_enum.tag = type_enum_field->value;
//                const_val->data.x_enum.payload = value_const_val;
//            } else {
//                if (context->fn_entry) {
//                    context->fn_entry->struct_val_expr_alloca_list.append(codegen);
//                } else {
//                    add_node_error(g, *value_node_ptr, buf_sprintf("unable to evaluate constant expression"));
//                    return g->builtin_types.entry_invalid;
//                }
//            }
//        } else if (type_enum_field->type_entry->id != TypeTableEntryIdVoid) {
//            add_node_error(g, field_access_node,
//                buf_sprintf("enum value '%s.%s' requires parameter of type '%s'",
//                    buf_ptr(&enum_type->name),
//                    buf_ptr(field_name),
//                    buf_ptr(&type_enum_field->type_entry->name)));
//        } else {
//            Expr *expr = get_resolved_expr(out_node);
//            expr->const_val.ok = true;
//            expr->const_val.data.x_enum.tag = type_enum_field->value;
//            expr->const_val.data.x_enum.payload = nullptr;
//        }
//    } else {
//        add_node_error(g, field_access_node,
//            buf_sprintf("no member named '%s' in '%s'", buf_ptr(field_name),
//                buf_ptr(&enum_type->name)));
//    }
//    return enum_type;
//}
//
//static TypeTableEntry *analyze_container_member_access_inner(CodeGen *g,
//    TypeTableEntry *bare_struct_type, Buf *field_name, AstNode *node, TypeTableEntry *struct_type)
//{
//    assert(node->type == NodeTypeFieldAccessExpr);
//    if (!is_slice(bare_struct_type)) {
//        BlockContext *container_block_context = get_container_block_context(bare_struct_type);
//        assert(container_block_context);
//        auto entry = container_block_context->decl_table.maybe_get(field_name);
//        AstNode *fn_decl_node = entry ? entry->value : nullptr;
//        if (fn_decl_node && fn_decl_node->type == NodeTypeFnProto) {
//            resolve_top_level_decl(g, fn_decl_node, false);
//            TopLevelDecl *tld = get_as_top_level_decl(fn_decl_node);
//            if (tld->resolution == TldResolutionInvalid) {
//                return g->builtin_types.entry_invalid;
//            }
//
//            node->data.field_access_expr.is_member_fn = true;
//            FnTableEntry *fn_entry = fn_decl_node->data.fn_proto.fn_table_entry;
//            if (fn_entry->type_entry->id == TypeTableEntryIdGenericFn) {
//                return resolve_expr_const_val_as_generic_fn(g, node, fn_entry->type_entry, false);
//            } else {
//                return resolve_expr_const_val_as_fn(g, node, fn_entry, false);
//            }
//        }
//    }
//    add_node_error(g, node,
//        buf_sprintf("no member named '%s' in '%s'", buf_ptr(field_name), buf_ptr(&bare_struct_type->name)));
//    return g->builtin_types.entry_invalid;
//}
//
//static TypeTableEntry *analyze_container_member_access(CodeGen *g,
//        Buf *field_name, AstNode *node, TypeTableEntry *struct_type)
//{
//    TypeTableEntry *bare_type = container_ref_type(struct_type);
//    if (!type_is_complete(bare_type)) {
//        resolve_container_type(g, bare_type);
//    }
//
//    node->data.field_access_expr.bare_container_type = bare_type;
//
//    if (bare_type->id == TypeTableEntryIdStruct) {
//        node->data.field_access_expr.type_struct_field = find_struct_type_field(bare_type, field_name);
//        if (node->data.field_access_expr.type_struct_field) {
//            return node->data.field_access_expr.type_struct_field->type_entry;
//        } else {
//            return analyze_container_member_access_inner(g, bare_type, field_name,
//                node, struct_type);
//        }
//    } else if (bare_type->id == TypeTableEntryIdEnum) {
//        node->data.field_access_expr.type_enum_field = find_enum_type_field(bare_type, field_name);
//        if (node->data.field_access_expr.type_enum_field) {
//            return node->data.field_access_expr.type_enum_field->type_entry;
//        } else {
//            return analyze_container_member_access_inner(g, bare_type, field_name,
//                node, struct_type);
//        }
//    } else if (bare_type->id == TypeTableEntryIdUnion) {
//        zig_panic("TODO");
//    } else {
//        zig_unreachable();
//    }
//}
//
//static TypeTableEntry *analyze_slice_expr(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        AstNode *node)
//{
//    assert(node->type == NodeTypeSliceExpr);
//
//    TypeTableEntry *array_type = analyze_expression(g, import, context, nullptr,
//            node->data.slice_expr.array_ref_expr);
//
//    TypeTableEntry *return_type;
//
//    if (array_type->id == TypeTableEntryIdInvalid) {
//        return_type = g->builtin_types.entry_invalid;
//    } else if (array_type->id == TypeTableEntryIdArray) {
//        return_type = get_slice_type(g, array_type->data.array.child_type,
//                node->data.slice_expr.is_const);
//    } else if (array_type->id == TypeTableEntryIdPointer) {
//        return_type = get_slice_type(g, array_type->data.pointer.child_type,
//                node->data.slice_expr.is_const);
//    } else if (array_type->id == TypeTableEntryIdStruct &&
//               array_type->data.structure.is_slice)
//    {
//        return_type = get_slice_type(g,
//                array_type->data.structure.fields[0].type_entry->data.pointer.child_type,
//                node->data.slice_expr.is_const);
//    } else {
//        add_node_error(g, node,
//            buf_sprintf("slice of non-array type '%s'", buf_ptr(&array_type->name)));
//        return_type = g->builtin_types.entry_invalid;
//    }
//
//    if (return_type->id != TypeTableEntryIdInvalid) {
//        node->data.slice_expr.resolved_struct_val_expr.type_entry = return_type;
//        node->data.slice_expr.resolved_struct_val_expr.source_node = node;
//        context->fn_entry->struct_val_expr_alloca_list.append(&node->data.slice_expr.resolved_struct_val_expr);
//    }
//
//    analyze_expression(g, import, context, g->builtin_types.entry_usize, node->data.slice_expr.start);
//
//    if (node->data.slice_expr.end) {
//        analyze_expression(g, import, context, g->builtin_types.entry_usize, node->data.slice_expr.end);
//    }
//
//    return return_type;
//}
//
//static TypeTableEntry *analyze_array_access_expr(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        AstNode *node, LValPurpose purpose)
//{
//    TypeTableEntry *array_type = analyze_expression(g, import, context, nullptr,
//            node->data.array_access_expr.array_ref_expr);
//
//    TypeTableEntry *return_type;
//
//    if (array_type->id == TypeTableEntryIdInvalid) {
//        return_type = g->builtin_types.entry_invalid;
//    } else if (array_type->id == TypeTableEntryIdArray) {
//        if (array_type->data.array.len == 0) {
//            add_node_error(g, node, buf_sprintf("out of bounds array access"));
//        }
//        return_type = array_type->data.array.child_type;
//    } else if (array_type->id == TypeTableEntryIdPointer) {
//        if (array_type->data.pointer.is_const && purpose == LValPurposeAssign) {
//            add_node_error(g, node, buf_sprintf("cannot assign to constant"));
//            return g->builtin_types.entry_invalid;
//        }
//        return_type = array_type->data.pointer.child_type;
//    } else if (array_type->id == TypeTableEntryIdStruct &&
//               array_type->data.structure.is_slice)
//    {
//        TypeTableEntry *pointer_type = array_type->data.structure.fields[0].type_entry;
//        if (pointer_type->data.pointer.is_const && purpose == LValPurposeAssign) {
//            add_node_error(g, node, buf_sprintf("cannot assign to constant"));
//            return g->builtin_types.entry_invalid;
//        }
//        return_type = pointer_type->data.pointer.child_type;
//    } else {
//        add_node_error(g, node,
//                buf_sprintf("array access of non-array type '%s'", buf_ptr(&array_type->name)));
//        return_type = g->builtin_types.entry_invalid;
//    }
//
//    analyze_expression(g, import, context, g->builtin_types.entry_usize, node->data.array_access_expr.subscript);
//
//    return return_type;
//}
//
//static TypeTableEntry *analyze_logic_bin_op_expr(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        AstNode *node)
//{
//    assert(node->type == NodeTypeBinOpExpr);
//    BinOpType bin_op_type = node->data.bin_op_expr.bin_op;
//
//    AstNode *op1 = node->data.bin_op_expr.op1;
//    AstNode *op2 = node->data.bin_op_expr.op2;
//    TypeTableEntry *op1_type = analyze_expression(g, import, context, g->builtin_types.entry_bool, op1);
//    TypeTableEntry *op2_type = analyze_expression(g, import, context, g->builtin_types.entry_bool, op2);
//
//    if (op1_type->id == TypeTableEntryIdInvalid ||
//        op2_type->id == TypeTableEntryIdInvalid)
//    {
//        return g->builtin_types.entry_invalid;
//    }
//
//    ConstExprValue *op1_val = &get_resolved_expr(op1)->const_val;
//    ConstExprValue *op2_val = &get_resolved_expr(op2)->const_val;
//    if (!op1_val->ok || !op2_val->ok) {
//        return g->builtin_types.entry_bool;
//    }
//
//    ConstExprValue *out_val = &get_resolved_expr(node)->const_val;
//    eval_const_expr_bin_op(op1_val, op1_type, bin_op_type, op2_val, op2_type, out_val);
//    return g->builtin_types.entry_bool;
//}
//
//static TypeTableEntry *analyze_array_mult(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        TypeTableEntry *expected_type, AstNode *node)
//{
//    assert(node->type == NodeTypeBinOpExpr);
//    assert(node->data.bin_op_expr.bin_op == BinOpTypeArrayMult);
//
//    AstNode **op1 = node->data.bin_op_expr.op1->parent_field;
//    AstNode **op2 = node->data.bin_op_expr.op2->parent_field;
//
//    TypeTableEntry *op1_type = analyze_expression(g, import, context, nullptr, *op1);
//    TypeTableEntry *op2_type = analyze_expression(g, import, context, nullptr, *op2);
//
//    if (op1_type->id == TypeTableEntryIdInvalid ||
//        op2_type->id == TypeTableEntryIdInvalid)
//    {
//        return g->builtin_types.entry_invalid;
//    }
//
//    ConstExprValue *op1_val = &get_resolved_expr(*op1)->const_val;
//    ConstExprValue *op2_val = &get_resolved_expr(*op2)->const_val;
//
//    AstNode *bad_node;
//    if (!op1_val->ok) {
//        bad_node = *op1;
//    } else if (!op2_val->ok) {
//        bad_node = *op2;
//    } else {
//        bad_node = nullptr;
//    }
//    if (bad_node) {
//        add_node_error(g, bad_node, buf_sprintf("array multiplication requires constant expression"));
//        return g->builtin_types.entry_invalid;
//    }
//
//    if (op1_type->id != TypeTableEntryIdArray) {
//        add_node_error(g, *op1,
//            buf_sprintf("expected array type, got '%s'", buf_ptr(&op1_type->name)));
//        return g->builtin_types.entry_invalid;
//    }
//
//    if (op2_type->id != TypeTableEntryIdNumLitInt &&
//        op2_type->id != TypeTableEntryIdInt)
//    {
//        add_node_error(g, *op2, buf_sprintf("expected integer type, got '%s'", buf_ptr(&op2_type->name)));
//        return g->builtin_types.entry_invalid;
//    }
//
//    if (op2_val->data.x_bignum.is_negative) {
//        add_node_error(g, *op2, buf_sprintf("expected positive number"));
//        return g->builtin_types.entry_invalid;
//    }
//
//    ConstExprValue *const_val = &get_resolved_expr(node)->const_val;
//    const_val->ok = true;
//    const_val->depends_on_compile_var = op1_val->depends_on_compile_var || op2_val->depends_on_compile_var;
//
//    TypeTableEntry *child_type = op1_type->data.array.child_type;
//    BigNum old_array_len;
//    bignum_init_unsigned(&old_array_len, op1_type->data.array.len);
//
//    BigNum new_array_len;
//    if (bignum_mul(&new_array_len, &old_array_len, &op2_val->data.x_bignum)) {
//        add_node_error(g, node, buf_sprintf("operation results in overflow"));
//        return g->builtin_types.entry_invalid;
//    }
//
//    uint64_t old_array_len_bare = op1_type->data.array.len;
//    uint64_t operand_amt = op2_val->data.x_bignum.data.x_uint;
//
//    uint64_t new_array_len_bare = new_array_len.data.x_uint;
//    const_val->data.x_array.fields = allocate<ConstExprValue*>(new_array_len_bare);
//
//    uint64_t i = 0;
//    for (uint64_t x = 0; x < operand_amt; x += 1) {
//        for (uint64_t y = 0; y < old_array_len_bare; y += 1) {
//            const_val->data.x_array.fields[i] = op1_val->data.x_array.fields[y];
//            i += 1;
//        }
//    }
//
//    return get_array_type(g, child_type, new_array_len_bare);
//}
//
//static TypeTableEntry *analyze_unwrap_error_expr(CodeGen *g, ImportTableEntry *import,
//        BlockContext *parent_context, TypeTableEntry *expected_type, AstNode *node)
//{
//    AstNode *op1 = node->data.unwrap_err_expr.op1;
//    AstNode *op2 = node->data.unwrap_err_expr.op2;
//    AstNode *var_node = node->data.unwrap_err_expr.symbol;
//
//    TypeTableEntry *lhs_type = analyze_expression(g, import, parent_context, nullptr, op1);
//    if (lhs_type->id == TypeTableEntryIdInvalid) {
//        return lhs_type;
//    } else if (lhs_type->id == TypeTableEntryIdErrorUnion) {
//        TypeTableEntry *child_type = lhs_type->data.error.child_type;
//        BlockContext *child_context;
//        if (var_node) {
//            child_context = new_block_context(node, parent_context);
//            var_node->block_context = child_context;
//            Buf *var_name = var_node->data.symbol_expr.symbol;
//            node->data.unwrap_err_expr.var = add_local_var(g, var_node, import, child_context, var_name,
//                    g->builtin_types.entry_pure_error, true, nullptr);
//        } else {
//            child_context = parent_context;
//        }
//
//        analyze_expression(g, import, child_context, child_type, op2);
//        return child_type;
//    } else {
//        add_node_error(g, op1,
//            buf_sprintf("expected error type, got '%s'", buf_ptr(&lhs_type->name)));
//        return g->builtin_types.entry_invalid;
//    }
//}
//
//
//static VariableTableEntry *analyze_variable_declaration_raw(CodeGen *g, ImportTableEntry *import,
//        BlockContext *context, AstNode *source_node,
//        AstNodeVariableDeclaration *variable_declaration,
//        bool expr_is_maybe, AstNode *decl_node, bool var_is_ptr)
//{
//    bool is_const = variable_declaration->is_const;
//    bool is_export = (variable_declaration->top_level_decl.visib_mod == VisibModExport);
//    bool is_extern = variable_declaration->is_extern;
//
//    TypeTableEntry *explicit_type = nullptr;
//    if (variable_declaration->type != nullptr) {
//        explicit_type = analyze_type_expr(g, import, context, variable_declaration->type);
//        if (explicit_type->id == TypeTableEntryIdUnreachable) {
//            add_node_error(g, variable_declaration->type,
//                buf_sprintf("variable of type 'unreachable' not allowed"));
//            explicit_type = g->builtin_types.entry_invalid;
//        }
//    }
//
//    TypeTableEntry *implicit_type = nullptr;
//    if (explicit_type && explicit_type->id == TypeTableEntryIdInvalid) {
//        implicit_type = explicit_type;
//    } else if (variable_declaration->expr) {
//        implicit_type = analyze_expression(g, import, context, explicit_type, variable_declaration->expr);
//        if (implicit_type->id == TypeTableEntryIdInvalid) {
//            // ignore the poison value
//        } else if (expr_is_maybe) {
//            if (implicit_type->id == TypeTableEntryIdMaybe) {
//                if (var_is_ptr) {
//                    // TODO if the expression is constant, can't get pointer to it
//                    implicit_type = get_pointer_to_type(g, implicit_type->data.maybe.child_type, false);
//                } else {
//                    implicit_type = implicit_type->data.maybe.child_type;
//                }
//            } else {
//                add_node_error(g, variable_declaration->expr, buf_sprintf("expected maybe type"));
//                implicit_type = g->builtin_types.entry_invalid;
//            }
//        } else if (implicit_type->id == TypeTableEntryIdUnreachable) {
//            add_node_error(g, source_node,
//                buf_sprintf("variable initialization is unreachable"));
//            implicit_type = g->builtin_types.entry_invalid;
//        } else if ((!is_const || is_export) &&
//                (implicit_type->id == TypeTableEntryIdNumLitFloat ||
//                 implicit_type->id == TypeTableEntryIdNumLitInt))
//        {
//            add_node_error(g, source_node, buf_sprintf("unable to infer variable type"));
//            implicit_type = g->builtin_types.entry_invalid;
//        } else if (implicit_type->id == TypeTableEntryIdMetaType && !is_const) {
//            add_node_error(g, source_node, buf_sprintf("variable of type 'type' must be constant"));
//            implicit_type = g->builtin_types.entry_invalid;
//        }
//        if (implicit_type->id != TypeTableEntryIdInvalid && !context->fn_entry) {
//            ConstExprValue *const_val = &get_resolved_expr(variable_declaration->expr)->const_val;
//            if (!const_val->ok) {
//                add_node_error(g, first_executing_node(variable_declaration->expr),
//                        buf_sprintf("global variable initializer requires constant expression"));
//            }
//        }
//    } else if (!is_extern) {
//        add_node_error(g, source_node, buf_sprintf("variables must be initialized"));
//        implicit_type = g->builtin_types.entry_invalid;
//    }
//
//    TypeTableEntry *type = explicit_type != nullptr ? explicit_type : implicit_type;
//    assert(type != nullptr); // should have been caught by the parser
//
//    VariableTableEntry *var = add_local_var(g, source_node, import, context,
//            variable_declaration->symbol, type, is_const,
//            expr_is_maybe ? nullptr : variable_declaration->expr);
//
//    variable_declaration->variable = var;
//
//    return var;
//}
//
//static VariableTableEntry *analyze_variable_declaration(CodeGen *g, ImportTableEntry *import,
//        BlockContext *context, TypeTableEntry *expected_type, AstNode *node)
//{
//    AstNodeVariableDeclaration *variable_declaration = &node->data.variable_declaration;
//    return analyze_variable_declaration_raw(g, import, context, node, variable_declaration,
//            false, nullptr, false);
//}
//
//
//static TypeTableEntry *analyze_while_expr(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        TypeTableEntry *expected_type, AstNode *node)
//{
//    assert(node->type == NodeTypeWhileExpr);
//
//    AstNode **condition_node = &node->data.while_expr.condition;
//    AstNode *while_body_node = node->data.while_expr.body;
//    AstNode **continue_expr_node = &node->data.while_expr.continue_expr;
//
//    TypeTableEntry *condition_type = analyze_expression(g, import, context,
//            g->builtin_types.entry_bool, *condition_node);
//
//    if (*continue_expr_node) {
//        analyze_expression(g, import, context, g->builtin_types.entry_void, *continue_expr_node);
//    }
//
//    BlockContext *child_context = new_block_context(node, context);
//    child_context->parent_loop_node = node;
//
//    analyze_expression(g, import, child_context, g->builtin_types.entry_void, while_body_node);
//
//
//    TypeTableEntry *expr_return_type = g->builtin_types.entry_void;
//
//    if (condition_type->id == TypeTableEntryIdInvalid) {
//        expr_return_type = g->builtin_types.entry_invalid;
//    } else {
//        // if the condition is a simple constant expression and there are no break statements
//        // then the return type is unreachable
//        ConstExprValue *const_val = &get_resolved_expr(*condition_node)->const_val;
//        if (const_val->ok) {
//            if (const_val->data.x_bool) {
//                node->data.while_expr.condition_always_true = true;
//                if (!node->data.while_expr.contains_break) {
//                    expr_return_type = g->builtin_types.entry_unreachable;
//                }
//            }
//        }
//    }
//
//    return expr_return_type;
//}
//
//static TypeTableEntry *analyze_break_expr(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        TypeTableEntry *expected_type, AstNode *node)
//{
//    assert(node->type == NodeTypeBreak);
//
//    AstNode *loop_node = context->parent_loop_node;
//    if (loop_node) {
//        if (loop_node->type == NodeTypeWhileExpr) {
//            loop_node->data.while_expr.contains_break = true;
//        } else if (loop_node->type == NodeTypeForExpr) {
//            loop_node->data.for_expr.contains_break = true;
//        } else {
//            zig_unreachable();
//        }
//    } else {
//        add_node_error(g, node, buf_sprintf("'break' expression outside loop"));
//    }
//    return g->builtin_types.entry_unreachable;
//}
//
//static TypeTableEntry *analyze_continue_expr(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        TypeTableEntry *expected_type, AstNode *node)
//{
//    AstNode *loop_node = context->parent_loop_node;
//    if (loop_node) {
//        if (loop_node->type == NodeTypeWhileExpr) {
//            loop_node->data.while_expr.contains_continue = true;
//        } else if (loop_node->type == NodeTypeForExpr) {
//            loop_node->data.for_expr.contains_continue = true;
//        } else {
//            zig_unreachable();
//        }
//    } else {
//        add_node_error(g, node, buf_sprintf("'continue' expression outside loop"));
//    }
//    return g->builtin_types.entry_unreachable;
//}
//
//static TypeTableEntry *analyze_defer(CodeGen *g, ImportTableEntry *import, BlockContext *parent_context,
//        TypeTableEntry *expected_type, AstNode *node)
//{
//    if (!parent_context->fn_entry) {
//        add_node_error(g, node, buf_sprintf("defer expression outside function definition"));
//        return g->builtin_types.entry_invalid;
//    }
//
//    if (!node->data.defer.expr) {
//        add_node_error(g, node, buf_sprintf("defer expects an expression"));
//        return g->builtin_types.entry_void;
//    }
//
//    node->data.defer.child_block = new_block_context(node, parent_context);
//
//    TypeTableEntry *resolved_type = analyze_expression(g, import, parent_context, nullptr,
//            node->data.defer.expr);
//    validate_voided_expr(g, node->data.defer.expr, resolved_type);
//
//    return g->builtin_types.entry_void;
//}
//
//static TypeTableEntry *analyze_string_literal_expr(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        TypeTableEntry *expected_type, AstNode *node)
//{
//    if (node->data.string_literal.c) {
//        return resolve_expr_const_val_as_c_string_lit(g, node, node->data.string_literal.buf);
//    } else {
//        return resolve_expr_const_val_as_string_lit(g, node, node->data.string_literal.buf);
//    }
//}
//
//static TypeTableEntry *analyze_block_expr(CodeGen *g, ImportTableEntry *import, BlockContext *parent_context,
//        TypeTableEntry *expected_type, AstNode *node)
//{
//    BlockContext *child_context = new_block_context(node, parent_context);
//    node->data.block.child_block = child_context;
//    TypeTableEntry *return_type = g->builtin_types.entry_void;
//
//    for (size_t i = 0; i < node->data.block.statements.length; i += 1) {
//        AstNode *child = node->data.block.statements.at(i);
//        if (child->type == NodeTypeLabel) {
//            FnTableEntry *fn_table_entry = child_context->fn_entry;
//            assert(fn_table_entry);
//
//            LabelTableEntry *label = allocate<LabelTableEntry>(1);
//            label->decl_node = child;
//            label->entered_from_fallthrough = (return_type->id != TypeTableEntryIdUnreachable);
//
//            child->block_context = child_context;
//            child->data.label.label_entry = label;
//            fn_table_entry->all_labels.append(label);
//
//            child_context->label_table.put(child->data.label.name, label);
//
//            return_type = g->builtin_types.entry_void;
//            continue;
//        }
//        if (return_type->id == TypeTableEntryIdUnreachable) {
//            if (is_node_void_expr(child)) {
//                // {unreachable;void;void} is allowed.
//                // ignore void statements once we enter unreachable land.
//                analyze_expression(g, import, child_context, g->builtin_types.entry_void, child);
//                continue;
//            }
//            add_node_error(g, first_executing_node(child), buf_sprintf("unreachable code"));
//            break;
//        }
//        bool is_last = (i == node->data.block.statements.length - 1);
//        TypeTableEntry *passed_expected_type = is_last ? expected_type : nullptr;
//        return_type = analyze_expression(g, import, child_context, passed_expected_type, child);
//        if (child->type == NodeTypeDefer && return_type->id != TypeTableEntryIdInvalid) {
//            // defer starts a new block context
//            child_context = child->data.defer.child_block;
//            assert(child_context);
//        }
//        if (!is_last) {
//            validate_voided_expr(g, child, return_type);
//        }
//    }
//    node->data.block.nested_block = child_context;
//
//    ConstExprValue *const_val = &node->data.block.resolved_expr.const_val;
//    if (node->data.block.statements.length == 0) {
//        const_val->ok = true;
//    } else if (node->data.block.statements.length == 1) {
//        AstNode *only_node = node->data.block.statements.at(0);
//        ConstExprValue *other_const_val = &get_resolved_expr(only_node)->const_val;
//        if (other_const_val->ok) {
//            *const_val = *other_const_val;
//        }
//    }
//
//    return return_type;
//}
//
//static TypeTableEntry *analyze_asm_expr(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        TypeTableEntry *expected_type, AstNode *node)
//{
//    mark_impure_fn(g, context, node);
//
//    node->data.asm_expr.return_count = 0;
//    TypeTableEntry *return_type = g->builtin_types.entry_void;
//    for (size_t i = 0; i < node->data.asm_expr.output_list.length; i += 1) {
//        AsmOutput *asm_output = node->data.asm_expr.output_list.at(i);
//        if (asm_output->return_type) {
//            node->data.asm_expr.return_count += 1;
//            return_type = analyze_type_expr(g, import, context, asm_output->return_type);
//            if (node->data.asm_expr.return_count > 1) {
//                add_node_error(g, node,
//                    buf_sprintf("inline assembly allows up to one output value"));
//                break;
//            }
//        } else {
//            Buf *variable_name = asm_output->variable_name;
//            VariableTableEntry *var = find_variable(g, context, variable_name);
//            if (var) {
//                asm_output->variable = var;
//                return var->type;
//            } else {
//                add_node_error(g, node,
//                        buf_sprintf("use of undeclared identifier '%s'", buf_ptr(variable_name)));
//                return g->builtin_types.entry_invalid;
//            }
//        }
//    }
//    for (size_t i = 0; i < node->data.asm_expr.input_list.length; i += 1) {
//        AsmInput *asm_input = node->data.asm_expr.input_list.at(i);
//        analyze_expression(g, import, context, nullptr, asm_input->expr);
//    }
//
//    return return_type;
//}
//
//static TypeTableEntry *analyze_error_literal_expr(CodeGen *g, ImportTableEntry *import,
//        BlockContext *context, AstNode *node, Buf *err_name)
//{
//    auto err_table_entry = g->error_table.maybe_get(err_name);
//
//    if (err_table_entry) {
//        return resolve_expr_const_val_as_err(g, node, err_table_entry->value);
//    }
//
//    add_node_error(g, node,
//            buf_sprintf("use of undeclared error value '%s'", buf_ptr(err_name)));
//
//    return g->builtin_types.entry_invalid;
//}
//
//static TypeTableEntry *analyze_symbol_expr(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        TypeTableEntry *expected_type, AstNode *node, bool pointer_only)
//{
//    Buf *variable_name = node->data.symbol_expr.symbol;
//
//    auto primitive_table_entry = g->primitive_type_table.maybe_get(variable_name);
//    if (primitive_table_entry) {
//        return resolve_expr_const_val_as_type(g, node, primitive_table_entry->value, false);
//    }
//
//    VariableTableEntry *var = find_variable(g, context, variable_name);
//    if (var) {
//        TypeTableEntry *var_type = analyze_var_ref(g, node, var, context, false);
//        return var_type;
//    }
//
//    AstNode *decl_node = find_decl(context, variable_name);
//    if (decl_node) {
//        return analyze_decl_ref(g, node, decl_node, pointer_only, context, false);
//    }
//
//    if (import->any_imports_failed) {
//        // skip the error message since we had a failing import in this file
//        // if an import breaks we don't need 9999 undeclared identifier errors
//        return g->builtin_types.entry_invalid;
//    }
//
//    mark_impure_fn(g, context, node);
//    add_node_error(g, node, buf_sprintf("use of undeclared identifier '%s'", buf_ptr(variable_name)));
//    return g->builtin_types.entry_invalid;
//}
//
//static TypeTableEntry *analyze_decl_ref(CodeGen *g, AstNode *source_node, AstNode *decl_node,
//        bool pointer_only, BlockContext *block_context, bool depends_on_compile_var)
//{
//    resolve_top_level_decl(g, decl_node, pointer_only);
//    TopLevelDecl *tld = get_as_top_level_decl(decl_node);
//    if (tld->resolution == TldResolutionInvalid) {
//        return g->builtin_types.entry_invalid;
//    }
//
//    if (decl_node->type == NodeTypeVariableDeclaration) {
//        VariableTableEntry *var = decl_node->data.variable_declaration.variable;
//        return analyze_var_ref(g, source_node, var, block_context, depends_on_compile_var);
//    } else if (decl_node->type == NodeTypeFnProto) {
//        FnTableEntry *fn_entry = decl_node->data.fn_proto.fn_table_entry;
//        assert(fn_entry->type_entry);
//        if (fn_entry->type_entry->id == TypeTableEntryIdGenericFn) {
//            return resolve_expr_const_val_as_generic_fn(g, source_node, fn_entry->type_entry, depends_on_compile_var);
//        } else {
//            return resolve_expr_const_val_as_fn(g, source_node, fn_entry, depends_on_compile_var);
//        }
//    } else if (decl_node->type == NodeTypeContainerDecl) {
//        if (decl_node->data.struct_decl.generic_params.length > 0) {
//            TypeTableEntry *type_entry = decl_node->data.struct_decl.generic_fn_type;
//            assert(type_entry);
//            return resolve_expr_const_val_as_generic_fn(g, source_node, type_entry, depends_on_compile_var);
//        } else {
//            return resolve_expr_const_val_as_type(g, source_node, decl_node->data.struct_decl.type_entry,
//                    depends_on_compile_var);
//        }
//    } else if (decl_node->type == NodeTypeTypeDecl) {
//        return resolve_expr_const_val_as_type(g, source_node, decl_node->data.type_decl.child_type_entry,
//                depends_on_compile_var);
//    } else {
//        zig_unreachable();
//    }
//}
//
//static TypeTableEntry *analyze_var_ref(CodeGen *g, AstNode *source_node, VariableTableEntry *var,
//        BlockContext *context, bool depends_on_compile_var)
//{
//    get_resolved_expr(source_node)->variable = var;
//    if (!var_is_pure(var, context)) {
//        mark_impure_fn(g, context, source_node);
//    }
//    if (var->src_is_const && var->val_node) {
//        ConstExprValue *other_const_val = &get_resolved_expr(var->val_node)->const_val;
//        if (other_const_val->ok) {
//            return resolve_expr_const_val_as_other_expr(g, source_node, var->val_node,
//                    depends_on_compile_var || var->force_depends_on_compile_var);
//        }
//    }
//    return var->type;
//}
//
//static TypeTableEntry *analyze_null_literal_expr(CodeGen *g, ImportTableEntry *import,
//        BlockContext *block_context, TypeTableEntry *expected_type, AstNode *node)
//{
//    assert(node->type == NodeTypeNullLiteral);
//
//    ConstExprValue *const_val = &get_resolved_expr(node)->const_val;
//    const_val->ok = true;
//
//    return g->builtin_types.entry_null;
//}
//
//static TypeTableEntry *analyze_undefined_literal_expr(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        TypeTableEntry *expected_type, AstNode *node)
//{
//    assert(node->type == NodeTypeUndefinedLiteral);
//
//    Expr *expr = get_resolved_expr(node);
//    ConstExprValue *const_val = &expr->const_val;
//
//    const_val->ok = true;
//    const_val->special = ConstValSpecialUndef;
//
//    return expected_type ? expected_type : g->builtin_types.entry_undef;
//}
//
//static TypeTableEntry *analyze_zeroes_literal_expr(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        TypeTableEntry *expected_type, AstNode *node)
//{
//    Expr *expr = get_resolved_expr(node);
//    ConstExprValue *const_val = &expr->const_val;
//
//    const_val->ok = true;
//    const_val->special = ConstValSpecialZeroes;
//
//    return expected_type ? expected_type : g->builtin_types.entry_undef;
//}
//
//static TypeTableEntry *analyze_number_literal_expr(CodeGen *g, ImportTableEntry *import,
//        BlockContext *block_context, TypeTableEntry *expected_type, AstNode *node)
//{
//    return resolve_expr_const_val_as_bignum(g, node, expected_type, node->data.number_literal.bignum, false);
//}
//
//static TypeTableEntry *analyze_fn_proto_expr(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        TypeTableEntry *expected_type, AstNode *node)
//{
//    TypeTableEntry *type_entry = analyze_fn_proto_type(g, import, context, expected_type, node,
//            false, false, nullptr);
//
//    if (type_entry->id == TypeTableEntryIdInvalid) {
//        return type_entry;
//    }
//
//    return resolve_expr_const_val_as_type(g, node, type_entry, false);
//}
//
//static bool var_is_pure(VariableTableEntry *var, BlockContext *context) {
//    if (var->block_context->fn_entry == context->fn_entry) {
//        // variable was declared in the current function, so it's OK.
//        return true;
//    }
//    return var->src_is_const && var->type->deep_const;
//}
//
//static void validate_voided_expr(CodeGen *g, AstNode *source_node, TypeTableEntry *type_entry) {
//    if (type_entry->id == TypeTableEntryIdMetaType) {
//        add_node_error(g, first_executing_node(source_node), buf_sprintf("expected expression, found type"));
//    } else if (type_entry->id == TypeTableEntryIdErrorUnion) {
//        add_node_error(g, first_executing_node(source_node), buf_sprintf("statement ignores error value"));
//    }
//}
//
//static TypeTableEntry *analyze_array_type(CodeGen *g, ImportTableEntry *import, BlockContext *context,
//        TypeTableEntry *expected_type, AstNode *node)
//{
//    AstNode *size_node = node->data.array_type.size;
//
//    TypeTableEntry *child_type = analyze_type_expr_pointer_only(g, import, context,
//            node->data.array_type.child_type, true);
//
//    if (child_type->id == TypeTableEntryIdUnreachable) {
//        add_node_error(g, node, buf_create_from_str("array of unreachable not allowed"));
//        return g->builtin_types.entry_invalid;
//    } else if (child_type->id == TypeTableEntryIdInvalid) {
//        return g->builtin_types.entry_invalid;
//    }
//
//    if (size_node) {
//        child_type = analyze_type_expr(g, import, context, node->data.array_type.child_type);
//        TypeTableEntry *size_type = analyze_expression(g, import, context,
//                g->builtin_types.entry_usize, size_node);
//        if (size_type->id == TypeTableEntryIdInvalid) {
//            return g->builtin_types.entry_invalid;
//        }
//
//        ConstExprValue *const_val = &get_resolved_expr(size_node)->const_val;
//        if (const_val->ok) {
//            if (const_val->data.x_bignum.is_negative) {
//                add_node_error(g, size_node,
//                    buf_sprintf("array size %s is negative",
//                        buf_ptr(bignum_to_buf(&const_val->data.x_bignum))));
//                return g->builtin_types.entry_invalid;
//            } else {
//                return resolve_expr_const_val_as_type(g, node,
//                        get_array_type(g, child_type, const_val->data.x_bignum.data.x_uint), false);
//            }
//        } else if (context->fn_entry) {
//            return resolve_expr_const_val_as_type(g, node,
//                    get_slice_type(g, child_type, node->data.array_type.is_const), false);
//        } else {
//            add_node_error(g, first_executing_node(size_node),
//                    buf_sprintf("unable to evaluate constant expression"));
//            return g->builtin_types.entry_invalid;
//        }
//    } else {
//        TypeTableEntry *slice_type = get_slice_type(g, child_type, node->data.array_type.is_const);
//        return resolve_expr_const_val_as_type(g, node, slice_type, false);
//    }
//}
//static size_t get_conditional_defer_count(BlockContext *inner_block, BlockContext *outer_block) {
//    size_t result = 0;
//    while (inner_block != outer_block) {
//        if (inner_block->node->type == NodeTypeDefer &&
//           (inner_block->node->data.defer.kind == ReturnKindError ||
//            inner_block->node->data.defer.kind == ReturnKindMaybe))
//        {
//            result += 1;
//        }
//        inner_block = inner_block->parent;
//    }
//    return result;
//}


//static IrInstruction *ir_gen_return(IrBuilder *irb, AstNode *source_node, IrInstruction *value, ReturnKnowledge rk) {
//    BlockContext *defer_inner_block = source_node->block_context;
//    BlockContext *defer_outer_block = irb->node->block_context;
//    if (rk == ReturnKnowledgeUnknown) {
//        if (get_conditional_defer_count(defer_inner_block, defer_outer_block) > 0) {
//            // generate branching code that checks the return value and generates defers
//            // if the return value is error
//            zig_panic("TODO");
//        }
//    } else if (rk != ReturnKnowledgeSkipDefers) {
//        ir_gen_defers_for_block(irb, defer_inner_block, defer_outer_block,
//                rk == ReturnKnowledgeKnownError, rk == ReturnKnowledgeKnownNull);
//    }
//
//    return ir_build_return(irb, source_node, value);
//}
/*
static void analyze_goto_pass2(CodeGen *g, ImportTableEntry *import, AstNode *node) {
    assert(node->type == NodeTypeGoto);
    Buf *label_name = node->data.goto_expr.name;
    BlockContext *context = node->block_context;
    assert(context);
    LabelTableEntry *label = find_label(g, context, label_name);

    if (!label) {
        add_node_error(g, node, buf_sprintf("no label in scope named '%s'", buf_ptr(label_name)));
        return;
    }

    label->used = true;
    node->data.goto_expr.label_entry = label;
}

    for (size_t i = 0; i < fn_table_entry->goto_list.length; i += 1) {
        AstNode *goto_node = fn_table_entry->goto_list.at(i);
        assert(goto_node->type == NodeTypeGoto);
        analyze_goto_pass2(g, import, goto_node);
    }

    for (size_t i = 0; i < fn_table_entry->all_labels.length; i += 1) {
        LabelTableEntry *label = fn_table_entry->all_labels.at(i);
        if (!label->used) {
            add_node_error(g, label->decl_node,
                    buf_sprintf("label '%s' defined but not used",
                        buf_ptr(label->decl_node->data.label.name)));
        }
    }
*/

//static LabelTableEntry *find_label(CodeGen *g, BlockContext *orig_context, Buf *name) {
//    BlockContext *context = orig_context;
//    while (context && context->fn_entry) {
//        auto entry = context->label_table.maybe_get(name);
//        if (entry) {
//            return entry->value;
//        }
//        context = context->parent;
//    }
//    return nullptr;
//}

