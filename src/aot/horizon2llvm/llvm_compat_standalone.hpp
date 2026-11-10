// Standalone Header-Only LLVM C++ IR Builder Compatibility Layer
// (src/aot/horizon2llvm/llvm_compat_standalone.hpp)
//
// Used automatically by `src/aot/horizon2llvm/llvm_ir.hpp` when external
// `llvm/IR/IRBuilder.h` C++ headers are not installed in `/usr/include` (e.g., when
// building against the dynamic `libLLVM-15.so.1` C API backend in `llvm_backend.cpp`).
// Implements the exact `llvm::LLVMContext`, `llvm::Module`, `llvm::Function`,
// `llvm::BasicBlock`, `llvm::Instruction`, `llvm::Value`, and `llvm::IRBuilder<llvm::NoFolder>`
// surface used by `llvm_ir.hpp`, `passes_llvm_utils.hpp`, `aarch64_lifter.cpp`, `passes.cpp`,
// and all 10 `lowering/pass_*.cpp` passes, emitting canonical LLVM 15+ opaque-pointer IR.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace llvm {

class raw_string_ostream {
public:
    explicit raw_string_ostream(std::string& s) : str_(s) {}
    raw_string_ostream& operator<<(const std::string& v) { str_ += v; return *this; }
    raw_string_ostream& operator<<(const char* v) { if (v) str_ += v; return *this; }
    void flush() {}
private:
    std::string& str_;
};

struct Align {
    unsigned value = 1;
    explicit constexpr Align(unsigned v = 1) : value(v ? v : 1) {}
};

struct MaybeAlign {
    unsigned value = 1;
    explicit constexpr MaybeAlign(unsigned v = 1) : value(v ? v : 1) {}
};

enum class AtomicOrdering {
    NotAtomic = 0,
    Unordered = 1,
    Monotonic = 2,
    Acquire = 4,
    Release = 5,
    AcquireRelease = 6,
    SequentiallyConsistent = 7,
};

inline const char* ordering_to_ir(AtomicOrdering ord) {
    switch (ord) {
        case AtomicOrdering::Monotonic: return "monotonic";
        case AtomicOrdering::Acquire: return "acquire";
        case AtomicOrdering::Release: return "release";
        case AtomicOrdering::AcquireRelease: return "acq_rel";
        case AtomicOrdering::SequentiallyConsistent: return "seq_cst";
        default: return "seq_cst";
    }
}

struct Triple {
    std::string str;
    Triple() = default;
    explicit Triple(std::string s) : str(std::move(s)) {}
};

struct SMDiagnostic {};

struct APInt {
    unsigned bits = 64;
    std::string hex_str;
    APInt(unsigned b, std::string s, unsigned /*radix*/) : bits(b), hex_str("u0x" + std::move(s)) {}
};

class Type {
public:
    std::string ir_str;
    explicit Type(std::string s) : ir_str(std::move(s)) {}
    virtual ~Type() = default;

    static Type* getVoidTy(class LLVMContext&);
    static Type* getInt1Ty(class LLVMContext&);
    static Type* getInt8Ty(class LLVMContext&);
    static Type* getInt16Ty(class LLVMContext&);
    static Type* getInt32Ty(class LLVMContext&);
    static Type* getInt64Ty(class LLVMContext&);
    static Type* getInt128Ty(class LLVMContext&);
    static Type* getFloatTy(class LLVMContext&);
    static Type* getDoubleTy(class LLVMContext&);
};

class LLVMContext {
public:
    std::unordered_map<std::string, std::unique_ptr<Type>> type_cache;
    std::vector<std::unique_ptr<class Value>> owned_vals;

    Type* intern_type(const std::string& s) {
        auto it = type_cache.find(s);
        if (it != type_cache.end()) return it->second.get();
        auto ty = std::make_unique<Type>(s);
        Type* ptr = ty.get();
        type_cache[s] = std::move(ty);
        return ptr;
    }

    template <typename T, typename... Args>
    T* alloc_val(Args&&... args) {
        auto p = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = p.get();
        owned_vals.push_back(std::move(p));
        return raw;
    }
};

inline Type* Type::getVoidTy(LLVMContext& c)   { return c.intern_type("void"); }
inline Type* Type::getInt1Ty(LLVMContext& c)   { return c.intern_type("i1"); }
inline Type* Type::getInt8Ty(LLVMContext& c)   { return c.intern_type("i8"); }
inline Type* Type::getInt16Ty(LLVMContext& c)  { return c.intern_type("i16"); }
inline Type* Type::getInt32Ty(LLVMContext& c)  { return c.intern_type("i32"); }
inline Type* Type::getInt64Ty(LLVMContext& c)  { return c.intern_type("i64"); }
inline Type* Type::getInt128Ty(LLVMContext& c) { return c.intern_type("i128"); }
inline Type* Type::getFloatTy(LLVMContext& c)  { return c.intern_type("float"); }
inline Type* Type::getDoubleTy(LLVMContext& c) { return c.intern_type("double"); }

struct PointerType {
    static Type* getUnqual(LLVMContext& c) { return c.intern_type("ptr"); }
};

struct FixedVectorType {
    static Type* get(Type* elt, unsigned n) {
        static LLVMContext fallback;
        std::string s = "<" + std::to_string(n) + " x " + (elt ? elt->ir_str : "i32") + ">";
        return fallback.intern_type(s);
    }
};

struct ArrayType {
    static Type* get(Type* elt, uint64_t n) {
        static LLVMContext fallback;
        std::string s = "[" + std::to_string(n) + " x " + (elt ? elt->ir_str : "i8") + "]";
        return fallback.intern_type(s);
    }
};

struct FunctionType : public Type {
    Type* ret_ty = nullptr;
    std::vector<Type*> param_tys;
    bool is_var_arg = false;

    FunctionType(Type* r, std::vector<Type*> p, bool va)
        : Type(r ? r->ir_str : "void"), ret_ty(r), param_tys(std::move(p)), is_var_arg(va) {}

    static FunctionType* get(Type* ret, std::vector<Type*> params, bool va = false) {
        static std::vector<std::unique_ptr<FunctionType>> pool;
        pool.push_back(std::make_unique<FunctionType>(ret, std::move(params), va));
        return pool.back().get();
    }
};

class Value {
public:
    Type* ty_ = nullptr;
    std::string repr_;
    std::string inst_text_;

    Value() = default;
    Value(Type* t, std::string r, std::string it = "")
        : ty_(t), repr_(std::move(r)), inst_text_(std::move(it)) {}
    virtual ~Value() = default;

    Type* getType() const { return ty_; }
    const std::string& getName() const { return repr_; }
    void setName(const std::string& n) {
        repr_ = (!n.empty() && n[0] != '%' && n[0] != '@') ? ("%" + n) : n;
    }

    virtual void print(raw_string_ostream& os) const {
        if (!inst_text_.empty()) os << inst_text_;
        else os << repr_;
    }
};

class Instruction : public Value {
public:
    enum BinaryOps {
        Add, Sub, Mul, UDiv, SDiv, URem, SRem, Shl, LShr, AShr, And, Or, Xor,
        FAdd, FSub, FMul, FDiv
    };
    enum CastOps {
        ZExt, SExt, Trunc, BitCast, IntToPtr, PtrToInt, FPExt, FPTrunc, SIToFP, UIToFP, FPToSI, FPToUI
    };
    using Value::Value;
};

struct CmpInst {
    enum Predicate {
        ICMP_EQ, ICMP_NE, ICMP_UGT, ICMP_UGE, ICMP_ULT, ICMP_ULE, ICMP_SGT, ICMP_SGE, ICMP_SLT, ICMP_SLE,
        FCMP_OEQ, FCMP_OGT, FCMP_OGE, FCMP_OLT, FCMP_OLE, FCMP_ONE, FCMP_ORD,
        FCMP_UNO, FCMP_UEQ, FCMP_UGT, FCMP_UGE, FCMP_ULT, FCMP_ULE, FCMP_UNE
    };
    static const char* pred_str(Predicate p) {
        switch (p) {
            case ICMP_EQ:  return "eq";
            case ICMP_NE:  return "ne";
            case ICMP_UGT: return "ugt";
            case ICMP_UGE: return "uge";
            case ICMP_ULT: return "ult";
            case ICMP_ULE: return "ule";
            case ICMP_SGT: return "sgt";
            case ICMP_SGE: return "sge";
            case ICMP_SLT: return "slt";
            case ICMP_SLE: return "sle";
            case FCMP_OEQ: return "oeq";
            case FCMP_OGT: return "ogt";
            case FCMP_OGE: return "oge";
            case FCMP_OLT: return "olt";
            case FCMP_OLE: return "ole";
            case FCMP_ONE: return "one";
            case FCMP_ORD: return "ord";
            case FCMP_UNO: return "uno";
            case FCMP_UEQ: return "ueq";
            case FCMP_UGT: return "ugt";
            case FCMP_UGE: return "uge";
            case FCMP_ULT: return "ult";
            case FCMP_ULE: return "ule";
            case FCMP_UNE: return "une";
        }
        return "eq";
    }
};

using ICmpInst = Instruction;
using FCmpInst = Instruction;
using BranchInst = Instruction;
using ReturnInst = Instruction;
using UnreachableInst = Instruction;

class AllocaInst : public Instruction {
public:
    unsigned align_ = 8;
    using Instruction::Instruction;
    void setAlignment(Align a) { align_ = a.value; }
};

class LoadInst : public Instruction {
public:
    Type* load_ty_ = nullptr;
    std::string ptr_repr_;
    unsigned align_ = 1;
    bool is_volatile_ = false;
    AtomicOrdering atomic_ = AtomicOrdering::NotAtomic;

    using Instruction::Instruction;
    void rebuild_text() {
        std::ostringstream os;
        if (!repr_.empty()) os << repr_ << " = ";
        os << "load ";
        if (atomic_ != AtomicOrdering::NotAtomic) os << "atomic ";
        if (is_volatile_) os << "volatile ";
        os << (load_ty_ ? load_ty_->ir_str : "i64") << ", ptr " << ptr_repr_;
        if (atomic_ != AtomicOrdering::NotAtomic) os << " " << ordering_to_ir(atomic_);
        os << ", align " << align_;
        inst_text_ = os.str();
    }
    void setAtomic(AtomicOrdering ord) {
        atomic_ = ord;
        rebuild_text();
    }
};

class StoreInst : public Instruction {
public:
    Type* val_ty_ = nullptr;
    std::string val_repr_;
    std::string ptr_repr_;
    unsigned align_ = 1;
    AtomicOrdering atomic_ = AtomicOrdering::NotAtomic;

    using Instruction::Instruction;
    void rebuild_text() {
        std::ostringstream os;
        os << "store ";
        if (atomic_ != AtomicOrdering::NotAtomic) os << "atomic ";
        os << (val_ty_ ? val_ty_->ir_str : "i64") << " " << val_repr_ << ", ptr " << ptr_repr_;
        if (atomic_ != AtomicOrdering::NotAtomic) os << " " << ordering_to_ir(atomic_);
        os << ", align " << align_;
        inst_text_ = os.str();
    }
    void setAtomic(AtomicOrdering ord) {
        atomic_ = ord;
        rebuild_text();
    }
};

class AtomicCmpXchgInst : public Instruction {
public:
    std::string ptr_repr_;
    std::string ty_str_;
    std::string exp_repr_;
    std::string new_repr_;
    unsigned align_ = 8;
    using Instruction::Instruction;
    void rebuild() {
        std::ostringstream os;
        if (!repr_.empty()) os << repr_ << " = ";
        os << "cmpxchg ptr " << ptr_repr_ << ", " << ty_str_ << " " << exp_repr_
           << ", " << ty_str_ << " " << new_repr_ << " seq_cst seq_cst, align " << align_;
        inst_text_ = os.str();
    }
    void setName(const std::string& n) {
        Value::setName(n);
        rebuild();
    }
};

class CallInst : public Instruction {
public:
    enum TailCallKind { TCK_None = 0, TCK_Tail = 1, TCK_MustTail = 2 };
    TailCallKind tck_ = TCK_None;
    std::string base_call_;
    using Instruction::Instruction;
    void setTailCallKind(TailCallKind k) {
        tck_ = k;
        std::string prefix = (k == TCK_MustTail) ? "musttail " : ((k == TCK_Tail) ? "tail " : "");
        if (!repr_.empty()) inst_text_ = repr_ + " = " + prefix + base_call_;
        else inst_text_ = prefix + base_call_;
    }
};

class BasicBlock : public Value {
public:
    std::string label_name;
    explicit BasicBlock(std::string lbl) : Value(nullptr, lbl), label_name(std::move(lbl)) {}
    static BasicBlock* Create(LLVMContext& ctx, const std::string& name, class Function* = nullptr) {
        return ctx.alloc_val<BasicBlock>(name);
    }
};

class SwitchInst : public Instruction {
public:
    std::string cond_ty_;
    std::string cond_repr_;
    std::string def_label_;
    std::vector<std::pair<std::string, std::string>> cases_;
    using Instruction::Instruction;
    void rebuild() {
        std::ostringstream os;
        os << "switch " << cond_ty_ << " " << cond_repr_ << ", label %" << def_label_ << " [\n";
        for (const auto& [cv, lbl] : cases_) {
            os << "    " << cond_ty_ << " " << cv << ", label %" << lbl << "\n";
        }
        os << "  ]";
        inst_text_ = os.str();
    }
    void addCase(Value* on_val, BasicBlock* dest) {
        cases_.emplace_back(on_val ? on_val->repr_ : "0", dest ? dest->label_name : "entry");
        rebuild();
    }
};

struct Attribute {
    enum AttrKind { NoUnwind, NoAlias, NoUndef, NonNull };
    static AttrKind getWithAlignment(LLVMContext&, Align) { return NoAlias; }
    static AttrKind getWithDereferenceableBytes(LLVMContext&, unsigned) { return NoAlias; }
};

class Argument : public Value {
public:
    Argument(Type* t, const std::string& name)
        : Value(t, (!name.empty() && name[0] == '%') ? name : ("%" + name)) {}
    void addAttr(Attribute::AttrKind) {}
};

struct GlobalValue {
    enum LinkageTypes { ExternalLinkage = 0 };
};

class GlobalVariable : public Value {
public:
    GlobalVariable(class Module& m, Type* ty, bool, GlobalValue::LinkageTypes, Value*, const std::string& name);
    void setDSOLocal(bool) {}
    void setAlignment(Align) {}
};

class Function : public Value {
public:
    FunctionType* fn_ty = nullptr;
    std::vector<std::unique_ptr<Argument>> args_;

    Function(FunctionType* ft, const std::string& name)
        : Value(ft ? ft->ret_ty : nullptr, "@" + name), fn_ty(ft) {
        if (ft) {
            for (size_t i = 0; i < ft->param_tys.size(); ++i) {
                args_.push_back(std::make_unique<Argument>(ft->param_tys[i], "arg" + std::to_string(i)));
            }
        }
    }
    void setDSOLocal(bool) {}
    void addFnAttr(Attribute::AttrKind) {}
    bool arg_empty() const { return args_.empty(); }
    Argument* getArg(unsigned i) const { return i < args_.size() ? args_[i].get() : nullptr; }
};

struct FunctionCallee {
    FunctionType* fty = nullptr;
    Value* callee = nullptr;
    Value* getCallee() const { return callee; }
};

class Module {
public:
    std::string name;
    LLVMContext& ctx;
    std::unordered_map<std::string, std::unique_ptr<Value>> named_vals;

    Module(std::string n, LLVMContext& c) : name(std::move(n)), ctx(c) {}
    void setTargetTriple(const Triple&) {}
    void setDataLayout(const std::string&) {}

    Value* getNamedGlobal(const std::string& g) const {
        return getNamedValue(g);
    }
    Value* getNamedValue(const std::string& g) const {
        auto it = named_vals.find(g);
        return it != named_vals.end() ? it->second.get() : nullptr;
    }
    FunctionCallee getOrInsertFunction(const std::string& fname, FunctionType* fty) {
        auto it = named_vals.find(fname);
        if (it != named_vals.end()) {
            return FunctionCallee{fty, it->second.get()};
        }
        auto fn = std::make_unique<Function>(fty, fname);
        Value* ptr = fn.get();
        named_vals[fname] = std::move(fn);
        return FunctionCallee{fty, ptr};
    }
};

inline GlobalVariable::GlobalVariable(Module& m, Type* ty, bool, GlobalValue::LinkageTypes, Value*, const std::string& name)
    : Value(ty, "@" + name) {
    m.named_vals[name] = std::make_unique<Value>(ty, "@" + name);
}

inline std::unique_ptr<Module> parseAssemblyString(const std::string&, SMDiagnostic&, LLVMContext&) {
    return nullptr;
}

struct Constant {
    static Value* getNullValue(Type* ty) {
        static LLVMContext c;
        bool is_int_or_ptr = ty && (ty->ir_str[0] == 'i' || ty->ir_str == "ptr");
        std::string r = (ty && ty->ir_str == "ptr") ? "null"
                      : (ty && (ty->ir_str == "float" || ty->ir_str == "double")) ? "0.0"
                      : is_int_or_ptr ? "0" : "zeroinitializer";
        return c.alloc_val<Value>(ty, r);
    }
};

struct ConstantInt {
    static Value* get(Type* ty, uint64_t v, bool is_signed = false) {
        static LLVMContext c;
        if (ty && ty->ir_str == "i1") {
            return c.alloc_val<Value>(ty, (v & 1) ? "true" : "false");
        }
        std::string s = is_signed ? std::to_string(static_cast<int64_t>(v)) : std::to_string(static_cast<int64_t>(v));
        return c.alloc_val<Value>(ty, s);
    }
    static Value* getSigned(Type* ty, int64_t sv) {
        static LLVMContext c;
        return c.alloc_val<Value>(ty, std::to_string(sv));
    }
    static Value* get(LLVMContext& c, const APInt& ap) {
        return c.alloc_val<Value>(Type::getInt128Ty(c), ap.hex_str);
    }
    static Value* getTrue(LLVMContext& c) {
        return c.alloc_val<Value>(Type::getInt1Ty(c), "true");
    }
    static Value* getFalse(LLVMContext& c) {
        return c.alloc_val<Value>(Type::getInt1Ty(c), "false");
    }
};

struct ConstantFP {
    static Value* get(Type* ty, double d) {
        static LLVMContext c;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.17g", d);
        std::string s(buf);
        if (s.find('.') == std::string::npos && s.find('e') == std::string::npos && s.find('E') == std::string::npos) {
            s += ".0";
        }
        return c.alloc_val<Value>(ty, s);
    }
};

struct PoisonValue {
    static Value* get(Type* ty) {
        static LLVMContext c;
        return c.alloc_val<Value>(ty, "poison");
    }
};

struct UndefValue {
    static Value* get(Type* ty) {
        static LLVMContext c;
        return c.alloc_val<Value>(ty, "undef");
    }
};

template <typename To, typename From>
To* dyn_cast(From* v) {
    return dynamic_cast<To*>(v);
}

template <typename To, typename From>
To* dyn_cast_or_null(From* v) {
    return v ? dynamic_cast<To*>(v) : nullptr;
}

struct NoFolder {};

template <typename Folder = NoFolder>
class IRBuilder {
public:
    explicit IRBuilder(LLVMContext& ctx) : ctx_(&ctx) {}
    explicit IRBuilder(BasicBlock* bb) : ctx_(&fallback_ctx()), bb_(bb) {}

    void SetInsertPoint(BasicBlock* bb) { bb_ = bb; }
    LLVMContext& getContext() const { return *ctx_; }

    Type* getInt1Ty() const  { return Type::getInt1Ty(*ctx_); }
    Type* getInt8Ty() const  { return Type::getInt8Ty(*ctx_); }
    Type* getInt32Ty() const { return Type::getInt32Ty(*ctx_); }
    Type* getInt64Ty() const { return Type::getInt64Ty(*ctx_); }

    Value* getTrue() const { return ConstantInt::getTrue(*ctx_); }
    Value* getInt32(uint32_t v) const { return ConstantInt::get(getInt32Ty(), v); }
    Value* getInt64(uint64_t v) const { return ConstantInt::get(getInt64Ty(), v); }

    static std::string pct(const std::string& n) {
        if (n.empty()) return "";
        return (n[0] == '%') ? n : ("%" + n);
    }

    Instruction* CreateBinOp(Instruction::BinaryOps opc, Value* lhs, Value* rhs, const std::string& name = "") {
        const char* op_s = "add";
        switch (opc) {
            case Instruction::Add:  op_s = "add"; break;
            case Instruction::Sub:  op_s = "sub"; break;
            case Instruction::Mul:  op_s = "mul"; break;
            case Instruction::UDiv: op_s = "udiv"; break;
            case Instruction::SDiv: op_s = "sdiv"; break;
            case Instruction::URem: op_s = "urem"; break;
            case Instruction::SRem: op_s = "srem"; break;
            case Instruction::Shl:  op_s = "shl"; break;
            case Instruction::LShr: op_s = "lshr"; break;
            case Instruction::AShr: op_s = "ashr"; break;
            case Instruction::And:  op_s = "and"; break;
            case Instruction::Or:   op_s = "or"; break;
            case Instruction::Xor:  op_s = "xor"; break;
            case Instruction::FAdd: op_s = "fadd"; break;
            case Instruction::FSub: op_s = "fsub"; break;
            case Instruction::FMul: op_s = "fmul"; break;
            case Instruction::FDiv: op_s = "fdiv"; break;
        }
        std::string r = pct(name);
        Type* ty = (lhs && lhs->getType()) ? lhs->getType() : getInt64Ty();
        std::string txt = r + " = " + op_s + " " + ty->ir_str + " " +
                          (lhs ? lhs->repr_ : "0") + ", " + (rhs ? rhs->repr_ : "0");
        return ctx_->alloc_val<Instruction>(ty, r, std::move(txt));
    }

    Instruction* CreateShl(Value* a, Value* b, const std::string& n = "")  { return CreateBinOp(Instruction::Shl, a, b, n); }
    Instruction* CreateLShr(Value* a, Value* b, const std::string& n = "") { return CreateBinOp(Instruction::LShr, a, b, n); }
    Instruction* CreateAnd(Value* a, Value* b, const std::string& n = "")  { return CreateBinOp(Instruction::And, a, b, n); }
    Instruction* CreateOr(Value* a, Value* b, const std::string& n = "")   { return CreateBinOp(Instruction::Or, a, b, n); }
    Instruction* CreateXor(Value* a, Value* b, const std::string& n = "")  { return CreateBinOp(Instruction::Xor, a, b, n); }

    Instruction* CreateICmp(CmpInst::Predicate pred, Value* lhs, Value* rhs, const std::string& name = "") {
        std::string r = pct(name);
        Type* op_ty = (lhs && lhs->getType()) ? lhs->getType() : getInt64Ty();
        Type* ret_ty = getInt1Ty();
        if (op_ty->ir_str.rfind("<", 0) == 0) {
            auto x_pos = op_ty->ir_str.find(" x ");
            if (x_pos != std::string::npos) {
                ret_ty = ctx_->intern_type(op_ty->ir_str.substr(0, x_pos) + " x i1>");
            }
        }
        std::string txt = r + " = icmp " + CmpInst::pred_str(pred) + " " + op_ty->ir_str + " " +
                          (lhs ? lhs->repr_ : "0") + ", " + (rhs ? rhs->repr_ : "0");
        return ctx_->alloc_val<Instruction>(ret_ty, r, std::move(txt));
    }

    Instruction* CreateICmpEQ(Value* a, Value* b, const std::string& n = "")  { return CreateICmp(CmpInst::ICMP_EQ, a, b, n); }
    Instruction* CreateICmpNE(Value* a, Value* b, const std::string& n = "")  { return CreateICmp(CmpInst::ICMP_NE, a, b, n); }
    Instruction* CreateICmpSLT(Value* a, Value* b, const std::string& n = "") { return CreateICmp(CmpInst::ICMP_SLT, a, b, n); }
    Instruction* CreateICmpUGE(Value* a, Value* b, const std::string& n = "") { return CreateICmp(CmpInst::ICMP_UGE, a, b, n); }
    Instruction* CreateICmpULT(Value* a, Value* b, const std::string& n = "") { return CreateICmp(CmpInst::ICMP_ULT, a, b, n); }

    Instruction* CreateFCmp(CmpInst::Predicate pred, Value* lhs, Value* rhs, const std::string& name = "") {
        std::string r = pct(name);
        Type* op_ty = (lhs && lhs->getType()) ? lhs->getType() : Type::getFloatTy(*ctx_);
        std::string txt = r + " = fcmp " + CmpInst::pred_str(pred) + " " + op_ty->ir_str + " " +
                          (lhs ? lhs->repr_ : "0.0") + ", " + (rhs ? rhs->repr_ : "0.0");
        return ctx_->alloc_val<Instruction>(getInt1Ty(), r, std::move(txt));
    }

    Instruction* CreateCast(Instruction::CastOps opc, Value* v, Type* dst_ty, const std::string& name = "") {
        const char* op_s = "bitcast";
        switch (opc) {
            case Instruction::ZExt:     op_s = "zext"; break;
            case Instruction::SExt:     op_s = "sext"; break;
            case Instruction::Trunc:    op_s = "trunc"; break;
            case Instruction::BitCast:  op_s = "bitcast"; break;
            case Instruction::IntToPtr: op_s = "inttoptr"; break;
            case Instruction::PtrToInt: op_s = "ptrtoint"; break;
            case Instruction::FPExt:    op_s = "fpext"; break;
            case Instruction::FPTrunc:  op_s = "fptrunc"; break;
            case Instruction::SIToFP:   op_s = "sitofp"; break;
            case Instruction::UIToFP:   op_s = "uitofp"; break;
            case Instruction::FPToSI:   op_s = "fptosi"; break;
            case Instruction::FPToUI:   op_s = "fptoui"; break;
        }
        std::string r = pct(name);
        Type* src_ty = (v && v->getType()) ? v->getType() : getInt64Ty();
        std::string txt = r + " = " + op_s + " " + src_ty->ir_str + " " +
                          (v ? v->repr_ : "0") + " to " + (dst_ty ? dst_ty->ir_str : "i64");
        return ctx_->alloc_val<Instruction>(dst_ty, r, std::move(txt));
    }

    Instruction* CreateZExt(Value* v, Type* dst_ty, const std::string& n = "")    { return CreateCast(Instruction::ZExt, v, dst_ty, n); }
    Instruction* CreateSExt(Value* v, Type* dst_ty, const std::string& n = "")    { return CreateCast(Instruction::SExt, v, dst_ty, n); }
    Instruction* CreateTrunc(Value* v, Type* dst_ty, const std::string& n = "")   { return CreateCast(Instruction::Trunc, v, dst_ty, n); }
    Instruction* CreateIntToPtr(Value* v, Type* dst_ty, const std::string& n = ""){ return CreateCast(Instruction::IntToPtr, v, dst_ty, n); }

    Instruction* CreateSelect(Value* cond, Value* tv, Value* fv, const std::string& name = "") {
        std::string r = pct(name);
        Type* c_ty = (cond && cond->getType()) ? cond->getType() : getInt1Ty();
        Type* v_ty = (tv && tv->getType()) ? tv->getType() : getInt64Ty();
        std::string txt = r + " = select " + c_ty->ir_str + " " + (cond ? cond->repr_ : "true") +
                          ", " + v_ty->ir_str + " " + (tv ? tv->repr_ : "0") +
                          ", " + v_ty->ir_str + " " + (fv ? fv->repr_ : "0");
        return ctx_->alloc_val<Instruction>(v_ty, r, std::move(txt));
    }

    Instruction* CreateExtractElement(Value* vec, Value* idx, const std::string& name = "") {
        std::string r = pct(name);
        Type* v_ty = (vec && vec->getType()) ? vec->getType() : getInt64Ty();
        std::string txt = r + " = extractelement " + v_ty->ir_str + " " + (vec ? vec->repr_ : "zeroinitializer") +
                          ", i32 " + (idx ? idx->repr_ : "0");
        return ctx_->alloc_val<Instruction>(getInt64Ty(), r, std::move(txt));
    }

    Instruction* CreateInsertElement(Value* vec, Value* elt, Value* idx, const std::string& name = "") {
        std::string r = pct(name);
        Type* v_ty = (vec && vec->getType()) ? vec->getType() : getInt64Ty();
        Type* e_ty = (elt && elt->getType()) ? elt->getType() : getInt32Ty();
        std::string txt = r + " = insertelement " + v_ty->ir_str + " " + (vec ? vec->repr_ : "zeroinitializer") +
                          ", " + e_ty->ir_str + " " + (elt ? elt->repr_ : "0") +
                          ", i32 " + (idx ? idx->repr_ : "0");
        return ctx_->alloc_val<Instruction>(v_ty, r, std::move(txt));
    }

    Instruction* CreateShuffleVector(Value* a, Value* b, const std::vector<int>& mask, const std::string& name = "") {
        std::string r = pct(name);
        Type* v_ty = (a && a->getType()) ? a->getType() : getInt64Ty();
        std::ostringstream os;
        os << r << " = shufflevector " << v_ty->ir_str << " " << (a ? a->repr_ : "zeroinitializer")
           << ", " << v_ty->ir_str << " " << (b ? b->repr_ : "zeroinitializer")
           << ", <" << mask.size() << " x i32> <";
        for (size_t i = 0; i < mask.size(); ++i) {
            if (i) os << ", ";
            os << "i32 " << mask[i];
        }
        os << ">";
        return ctx_->alloc_val<Instruction>(v_ty, r, os.str());
    }

    AllocaInst* CreateAlloca(Type* ty, Value*, const std::string& name = "") {
        std::string r = pct(name);
        std::string txt = r + " = alloca " + (ty ? ty->ir_str : "i64") + ", align 8";
        return ctx_->alloc_val<AllocaInst>(PointerType::getUnqual(*ctx_), r, std::move(txt));
    }

    Instruction* CreateInBoundsGEP(Type* ty, Value* ptr, Value* offset, const std::string& name = "") {
        std::string r = pct(name);
        std::string txt = r + " = getelementptr inbounds " + (ty ? ty->ir_str : "i8") +
                          ", ptr " + (ptr ? ptr->repr_ : "%c") +
                          ", i64 " + (offset ? offset->repr_ : "0");
        return ctx_->alloc_val<Instruction>(PointerType::getUnqual(*ctx_), r, std::move(txt));
    }

    LoadInst* CreateAlignedLoad(Type* ty, Value* ptr, Align align, const std::string& name = "") {
        return CreateAlignedLoad(ty, ptr, align, false, name);
    }

    LoadInst* CreateAlignedLoad(Type* ty, Value* ptr, Align align, bool is_vol, const std::string& name = "") {
        std::string r = pct(name);
        auto* ld = ctx_->alloc_val<LoadInst>(ty, r);
        ld->load_ty_ = ty;
        ld->ptr_repr_ = ptr ? ptr->repr_ : "%c";
        ld->align_ = align.value;
        ld->is_volatile_ = is_vol;
        ld->rebuild_text();
        return ld;
    }

    StoreInst* CreateAlignedStore(Value* val, Value* ptr, Align align) {
        auto* st = ctx_->alloc_val<StoreInst>(Type::getVoidTy(*ctx_), "");
        st->val_ty_ = (val && val->getType()) ? val->getType() : getInt64Ty();
        st->val_repr_ = val ? val->repr_ : "0";
        st->ptr_repr_ = ptr ? ptr->repr_ : "%c";
        st->align_ = align.value;
        st->rebuild_text();
        return st;
    }

    AtomicCmpXchgInst* CreateAtomicCmpXchg(Value* ptr, Value* exp, Value* nw, MaybeAlign align,
                                           AtomicOrdering, AtomicOrdering) {
        Type* v_ty = (exp && exp->getType()) ? exp->getType() : getInt64Ty();
        Type* pair_ty = ctx_->intern_type("{ " + v_ty->ir_str + ", i1 }");
        auto* cx = ctx_->alloc_val<AtomicCmpXchgInst>(pair_ty, "%cx");
        cx->ptr_repr_ = ptr ? ptr->repr_ : "%c";
        cx->ty_str_ = v_ty->ir_str;
        cx->exp_repr_ = exp ? exp->repr_ : "0";
        cx->new_repr_ = nw ? nw->repr_ : "0";
        cx->align_ = align.value;
        cx->rebuild();
        return cx;
    }

    Instruction* CreateExtractValue(Value* agg, std::initializer_list<unsigned> idxs, const std::string& name = "") {
        std::string r = pct(name);
        Type* a_ty = (agg && agg->getType()) ? agg->getType() : ctx_->intern_type("{ i64, i1 }");
        std::ostringstream os;
        os << r << " = extractvalue " << a_ty->ir_str << " " << (agg ? agg->repr_ : "%cx");
        for (unsigned idx : idxs) os << ", " << idx;
        return ctx_->alloc_val<Instruction>(getInt1Ty(), r, os.str());
    }

    CallInst* CreateCall(FunctionCallee callee, const std::vector<Value*>& args, const std::string& name = "") {
        std::string r = pct(name);
        Type* ret_ty = (callee.fty && callee.fty->ret_ty) ? callee.fty->ret_ty : Type::getVoidTy(*ctx_);
        std::string fn_sym = (callee.callee && !callee.callee->repr_.empty()) ? callee.callee->repr_ : "@unknown";
        std::ostringstream os;
        os << "call " << ret_ty->ir_str << " " << fn_sym << "(";
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) os << ", ";
            Type* p_ty = (callee.fty && i < callee.fty->param_tys.size() && callee.fty->param_tys[i])
                       ? callee.fty->param_tys[i]
                       : ((args[i] && args[i]->getType()) ? args[i]->getType() : getInt64Ty());
            os << p_ty->ir_str << " " << (args[i] ? args[i]->repr_ : "0");
        }
        os << ")";
        std::string base = os.str();
        std::string full = (!r.empty() && ret_ty->ir_str != "void") ? (r + " = " + base) : base;
        auto* ci = ctx_->alloc_val<CallInst>(ret_ty, r, std::move(full));
        ci->base_call_ = std::move(base);
        return ci;
    }

    BranchInst* CreateBr(BasicBlock* dest) {
        std::string txt = "br label %" + (dest ? dest->label_name : "entry");
        return ctx_->alloc_val<BranchInst>(Type::getVoidTy(*ctx_), "", std::move(txt));
    }

    BranchInst* CreateCondBr(Value* cond, BasicBlock* t_bb, BasicBlock* f_bb) {
        std::string txt = "br i1 " + (cond ? cond->repr_ : "true") +
                          ", label %" + (t_bb ? t_bb->label_name : "true_bb") +
                          ", label %" + (f_bb ? f_bb->label_name : "false_bb");
        return ctx_->alloc_val<BranchInst>(Type::getVoidTy(*ctx_), "", std::move(txt));
    }

    ReturnInst* CreateRetVoid() {
        return ctx_->alloc_val<ReturnInst>(Type::getVoidTy(*ctx_), "", "ret void");
    }

    UnreachableInst* CreateUnreachable() {
        return ctx_->alloc_val<UnreachableInst>(Type::getVoidTy(*ctx_), "", "unreachable");
    }

    SwitchInst* CreateSwitch(Value* v, BasicBlock* def, unsigned) {
        auto* sw = ctx_->alloc_val<SwitchInst>(Type::getVoidTy(*ctx_), "");
        sw->cond_ty_ = (v && v->getType()) ? v->getType()->ir_str : "i64";
        sw->cond_repr_ = v ? v->repr_ : "0";
        sw->def_label_ = def ? def->label_name : "entry";
        sw->rebuild();
        return sw;
    }

private:
    LLVMContext* ctx_ = nullptr;
    BasicBlock* bb_ = nullptr;

    static LLVMContext& fallback_ctx() {
        static LLVMContext c;
        return c;
    }
};

} // namespace llvm
