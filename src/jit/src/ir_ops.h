#pragma once

// IR building blocks with the exact semantics of pq_dsp's lane-width abstraction (src/dsp/src/simd.h) and of
// pq/core/fast_math.h's ops structs (T1b.1). One `Vx` emits either scalar code (W = 1: float / i32, the kernels'
// S1 path) or 8-wide vector code (W = 8: <8 x float> / <8 x i32>, the S8 path). Every primitive maps to the
// same x86 operation the kernels use, so generated code is bit-identical to the kernels except where fast-math
// contraction is allowed (CompileOptions::contract):
//   min/max          (a < b ? a : b) / (a > b ? a : b): minps/maxps, the second operand on NaN
//   select / blend   by the sign bit of the mask (blendvps); bool masks are canonical, so this is "mask ≠ 0"
//   truncToInt etc.  the x86 conversion intrinsics (NaN / out of range → INT_MIN), never fptosi (poison)
//   floor / round    llvm.floor / llvm.roundeven (roundps with _MM_FROUND_NO_EXC)
// `IrS` adapts a Vx to the static ops-struct interface of pq/core/fast_math.h, so fast_exp2/log2/sin/tan/tanh
// are generated from the very same template code as the native functions (same constants, same operation order).
//
// Not RT code: runs on the compiler thread.

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicsX86.h>

#include <bit>
#include <cstdint>

namespace pq::jit::detail {

class Vx {
public:
  Vx(llvm::IRBuilder<>& b, unsigned w) : b_(b), w_(w) {
    llvm::LLVMContext& c = b.getContext();
    f32_ = llvm::Type::getFloatTy(c);
    i32_ = llvm::Type::getInt32Ty(c);
    f64_ = llvm::Type::getDoubleTy(c);
    fT_ = w == 1 ? f32_ : static_cast<llvm::Type*>(llvm::FixedVectorType::get(f32_, w));
    iT_ = w == 1 ? i32_ : static_cast<llvm::Type*>(llvm::FixedVectorType::get(i32_, w));
    dT_ = w == 1 ? f64_ : static_cast<llvm::Type*>(llvm::FixedVectorType::get(f64_, w));
  }

  using V = llvm::Value*;
  unsigned width() const noexcept { return w_; }
  llvm::Type* fT() const noexcept { return fT_; }
  llvm::Type* iT() const noexcept { return iT_; }
  llvm::IRBuilder<>& b() noexcept { return b_; }

  // ---- constants ----
  V cf(float x) const { return cbits(std::bit_cast<uint32_t>(x)); }
  V cbits(uint32_t bits) const {  // a float with these exact bits (bool masks, NaN payloads)
    llvm::Constant* s = llvm::ConstantFP::get(b_.getContext(), llvm::APFloat(llvm::APFloat::IEEEsingle(), llvm::APInt(32, bits)));
    return w_ == 1 ? s : llvm::ConstantVector::getSplat(llvm::ElementCount::getFixed(w_), s);
  }
  V ci(int32_t x) const { return llvm::ConstantInt::get(iT_, static_cast<uint64_t>(static_cast<int64_t>(x)), true); }
  V zero() const { return cf(0.0f); }
  V ones() const { return cbits(0xFFFFFFFFu); }

  // ---- float arithmetic (the builder's fast-math flags apply: contract/afn or none) ----
  V add(V a, V c) { return b_.CreateFAdd(a, c); }
  V sub(V a, V c) { return b_.CreateFSub(a, c); }
  V mul(V a, V c) { return b_.CreateFMul(a, c); }
  V div(V a, V c) { return b_.CreateFDiv(a, c); }
  V min(V a, V c) { return b_.CreateSelect(b_.CreateFCmpOLT(a, c), a, c); }
  V max(V a, V c) { return b_.CreateSelect(b_.CreateFCmpOGT(a, c), a, c); }
  V fma(V a, V c, V d) { return b_.CreateIntrinsic(llvm::Intrinsic::fma, {fT_}, {a, c, d}); }
  V fnma(V a, V c, V d) { return fma(b_.CreateFNeg(a), c, d); }  // d − a·c, one rounding (vfnmadd)
  V sqrt(V a) { return b_.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, a); }
  V floor(V a) { return b_.CreateUnaryIntrinsic(llvm::Intrinsic::floor, a); }
  V round(V a) { return b_.CreateUnaryIntrinsic(llvm::Intrinsic::roundeven, a); }
  V abs(V a) { return b_.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, a); }
  V neg(V a) { return b_.CreateFNeg(a); }

  // ---- comparisons → masks (ordered, except ne) ----
  V mask(V i1) { return b_.CreateBitCast(b_.CreateSExt(i1, iT_), fT_); }
  V lt(V a, V c) { return mask(b_.CreateFCmpOLT(a, c)); }
  V le(V a, V c) { return mask(b_.CreateFCmpOLE(a, c)); }
  V gt(V a, V c) { return mask(b_.CreateFCmpOGT(a, c)); }
  V ge(V a, V c) { return mask(b_.CreateFCmpOGE(a, c)); }
  V eq(V a, V c) { return mask(b_.CreateFCmpOEQ(a, c)); }
  V ne(V a, V c) { return mask(b_.CreateFCmpUNE(a, c)); }
  V notLe(V a, V c) { return mask(b_.CreateFCmpUGT(a, c)); }  // _CMP_NLE_UQ: true on NaN

  // ---- bitwise on floats ----
  V asI(V f) { return b_.CreateBitCast(f, iT_); }
  V asF(V i) { return b_.CreateBitCast(i, fT_); }
  // On AVX-512 targets (kMasks(), set per generated function) a comparison's mask AND a value is a zero-masking
  // select on the comparison's k-register (same bits: all ones keeps the value, zero gives +0.0) instead of
  // materialising the mask in a vector register (T1b.2: bool masks as vector predicates).
  V band(V a, V c) {
    if (kMasks()) {
      if (V p = maskPred(a)) return b_.CreateSelect(p, c, zero());
      if (V p = maskPred(c)) return b_.CreateSelect(p, a, zero());
    }
    return asF(b_.CreateAnd(asI(a), asI(c)));
  }
  V bor(V a, V c) { return asF(b_.CreateOr(asI(a), asI(c))); }
  V bxor(V a, V c) { return asF(b_.CreateXor(asI(a), asI(c))); }
  V andnot(V a, V c) {  // ~a & c
    if (kMasks())
      if (V p = maskPred(a)) return b_.CreateSelect(p, zero(), c);
    return asF(b_.CreateAnd(b_.CreateNot(asI(a)), asI(c)));
  }
  static bool& kMasks() noexcept {
    thread_local bool on = false;
    return on;
  }
  // The comparison behind a mask made by mask(), or null.
  static V maskPred(V m) {
    if (auto* bc = llvm::dyn_cast<llvm::BitCastInst>(m))
      if (auto* se = llvm::dyn_cast<llvm::SExtInst>(bc->getOperand(0)))
        if (se->getSrcTy()->getScalarSizeInBits() == 1) return se->getOperand(0);
    return nullptr;
  }
  V bnot(V a) { return asF(b_.CreateNot(asI(a))); }
  // m ? a : c by the sign bit of m (blendvps). A mask that comes straight from a comparison selects by the
  // comparison itself (T1b.2: the predicate, a k-register on AVX-512, instead of a sign-extended lane mask that is
  // compared again); the result is the same because a comparison's mask is all ones or zero.
  V select(V m, V a, V c) { return b_.CreateSelect(pred(m), a, c); }
  V pred(V m) {
    if (V p = maskPred(m)) return p;
    return b_.CreateICmpSLT(asI(m), ci(0));
  }

  // ---- integer lanes ----
  V addi(V a, V c) { return b_.CreateAdd(a, c); }
  V subi(V a, V c) { return b_.CreateSub(a, c); }
  V muli(V a, V c) { return b_.CreateMul(a, c); }
  V andi(V a, V c) { return b_.CreateAnd(a, c); }
  V xori(V a, V c) { return b_.CreateXor(a, c); }
  V mini(V a, V c) { return b_.CreateBinaryIntrinsic(llvm::Intrinsic::smin, a, c); }
  V maxi(V a, V c) { return b_.CreateBinaryIntrinsic(llvm::Intrinsic::smax, a, c); }
  V slli(V a, int n) { return b_.CreateShl(a, ci(n)); }
  V srli(V a, int n) { return b_.CreateLShr(a, ci(n)); }
  V srai(V a, int n) { return b_.CreateAShr(a, ci(n)); }
  V eqi(V a, V c) { return b_.CreateSExt(b_.CreateICmpEQ(a, c), iT_); }
  V gti(V a, V c) { return b_.CreateSExt(b_.CreateICmpSGT(a, c), iT_); }
  V selecti(V m, V a, V c) {  // per-lane sign (a sign-extended comparison: the comparison, as in select)
    if (auto* se = llvm::dyn_cast<llvm::SExtInst>(m))
      if (se->getSrcTy()->getScalarSizeInBits() == 1) return b_.CreateSelect(se->getOperand(0), a, c);
    return b_.CreateSelect(b_.CreateICmpSLT(m, ci(0)), a, c);
  }
  V toFloat(V i) { return b_.CreateSIToFP(i, fT_); }

  // cvttps2dq / cvtps2dq: NaN and out-of-range → INT_MIN (the "integer indefinite" value).
  V truncToInt(V f) { return cvt(f, true); }
  V roundToInt(V f) { return cvt(f, false); }

  // round(x · scale) as int32 computed in double (dsp S8/S1::roundMulToInt: cvtps→pd, mulpd, cvtpd2dq).
  V roundMulToInt(V x, V scaleDouble) {
    llvm::Value* xd = b_.CreateFPExt(x, dT_);
    llvm::Value* sd = w_ == 1 ? scaleDouble : b_.CreateVectorSplat(w_, scaleDouble);
    llvm::Value* p;
    {
      llvm::IRBuilderBase::FastMathFlagGuard guard(b_);
      b_.clearFastMathFlags();
      p = b_.CreateFMul(xd, sd);
    }
    llvm::Module* m = b_.GetInsertBlock()->getModule();
    if (w_ == 1) {
      llvm::Type* v2d = llvm::FixedVectorType::get(f64_, 2);
      llvm::Value* v = b_.CreateInsertElement(llvm::PoisonValue::get(v2d), p, uint64_t{0});
      return b_.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(m, llvm::Intrinsic::x86_sse2_cvtsd2si), {v});
    }
    if (w_ == 16 && zmm()) {  // vcvtpd2dq zmm: the same conversion (current rounding mode, overflow → INT_MIN)
      llvm::Function* cvt = llvm::Intrinsic::getOrInsertDeclaration(m, llvm::Intrinsic::x86_avx512_mask_cvtpd2dq_512);
      llvm::Type* v8i = llvm::FixedVectorType::get(i32_, 8);
      auto part = [&](int base) {
        llvm::SmallVector<int, 8> idx;
        for (int k = 0; k < 8; ++k) idx.push_back(base + k);
        return b_.CreateCall(cvt, {b_.CreateShuffleVector(p, idx), llvm::Constant::getNullValue(v8i),
                                   b_.getInt8(0xFF), b_.getInt32(4)});
      };
      return concat16(part(0), part(8));
    }
    llvm::Function* cvt = llvm::Intrinsic::getOrInsertDeclaration(m, llvm::Intrinsic::x86_avx_cvt_pd2dq_256);
    if (w_ == 16) {
      llvm::Value* q[4];
      for (int k = 0; k < 4; ++k) q[k] = b_.CreateCall(cvt, {b_.CreateShuffleVector(p, {4 * k, 4 * k + 1, 4 * k + 2, 4 * k + 3})});
      return concat16(b_.CreateShuffleVector(q[0], q[1], {0, 1, 2, 3, 4, 5, 6, 7}),
                      b_.CreateShuffleVector(q[2], q[3], {0, 1, 2, 3, 4, 5, 6, 7}));
    }
    llvm::Value* lo = b_.CreateCall(cvt, {b_.CreateShuffleVector(p, {0, 1, 2, 3})});
    llvm::Value* hi = b_.CreateCall(cvt, {b_.CreateShuffleVector(p, {4, 5, 6, 7})});
    return b_.CreateShuffleVector(lo, hi, {0, 1, 2, 3, 4, 5, 6, 7});
  }

private:
  // A pair function on 512-bit registers (codegen.h): its <16 x …> conversions use the AVX-512 forms, which round
  // and saturate exactly like the 256-bit ones; on two 256-bit halves they use the AVX forms per half.
  bool zmm() const {
    const llvm::Function* f = b_.GetInsertBlock()->getParent();
    return f->getFnAttribute("prefer-vector-width").getValueAsString() == "512";
  }
  V concat16(V lo, V hi) {
    llvm::SmallVector<int, 16> idx;
    for (int k = 0; k < 16; ++k) idx.push_back(k);
    return b_.CreateShuffleVector(lo, hi, idx);
  }
  V halfOf(V x, int h) {
    llvm::SmallVector<int, 8> idx;
    for (int k = 0; k < 8; ++k) idx.push_back(h * 8 + k);
    return b_.CreateShuffleVector(x, idx);
  }
  V cvt(V f, bool trunc) {
    llvm::Module* m = b_.GetInsertBlock()->getModule();
    if (w_ == 1) {
      llvm::Type* v4f = llvm::FixedVectorType::get(f32_, 4);
      llvm::Value* v = b_.CreateInsertElement(llvm::PoisonValue::get(v4f), f, uint64_t{0});
      return b_.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                               m, trunc ? llvm::Intrinsic::x86_sse_cvttss2si : llvm::Intrinsic::x86_sse_cvtss2si),
                           {v});
    }
    if (w_ == 16 && zmm())
      return b_.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                               m, trunc ? llvm::Intrinsic::x86_avx512_mask_cvttps2dq_512 : llvm::Intrinsic::x86_avx512_mask_cvtps2dq_512),
                           {f, llvm::Constant::getNullValue(iT_), b_.getInt16(0xFFFF), b_.getInt32(4)});
    llvm::Function* c256 = llvm::Intrinsic::getOrInsertDeclaration(
        m, trunc ? llvm::Intrinsic::x86_avx_cvtt_ps2dq_256 : llvm::Intrinsic::x86_avx_cvt_ps2dq_256);
    if (w_ == 16) return concat16(b_.CreateCall(c256, {halfOf(f, 0)}), b_.CreateCall(c256, {halfOf(f, 1)}));
    return b_.CreateCall(c256, {f});
  }

  llvm::IRBuilder<>& b_;
  unsigned w_;
  llvm::Type *f32_, *i32_, *f64_, *fT_, *iT_, *dT_;
};

// pq/core/fast_math.h's ops-struct interface over the current Vx (set by IrScope; one per thread).
struct IrS {
  using F = llvm::Value*;
  using I = llvm::Value*;
  static Vx& v() noexcept { return *cur(); }
  static Vx*& cur() noexcept {
    thread_local Vx* p = nullptr;
    return p;
  }
  static F set(float x) { return v().cf(x); }
  static I seti(int32_t x) { return v().ci(x); }
  static F add(F a, F b) { return v().add(a, b); }
  static F sub(F a, F b) { return v().sub(a, b); }
  static F mul(F a, F b) { return v().mul(a, b); }
  static F div(F a, F b) { return v().div(a, b); }
  static F fma(F a, F b, F c) { return v().fma(a, b, c); }
  static F fnma(F a, F b, F c) { return v().fnma(a, b, c); }
  static F min(F a, F b) { return v().min(a, b); }
  static F max(F a, F b) { return v().max(a, b); }
  static F andnot(F a, F b) { return v().andnot(a, b); }
  static F band(F a, F b) { return v().band(a, b); }
  static F bor(F a, F b) { return v().bor(a, b); }
  static F bxor(F a, F b) { return v().bxor(a, b); }
  static F roundNearest(F x) { return v().round(x); }
  static I truncToInt(F x) { return v().truncToInt(x); }
  static F intToFloat(I x) { return v().toFloat(x); }
  static I addi(I a, I b) { return v().addi(a, b); }
  static I subi(I a, I b) { return v().subi(a, b); }
  template <int N> static I shl(I x) { return v().slli(x, N); }
  template <int N> static I sar(I x) { return v().srai(x, N); }
  static F asF(I x) { return v().asF(x); }
  static I asI(F x) { return v().asI(x); }
  static F lt(F a, F b) { return v().lt(a, b); }
  static F notLe(F a, F b) { return v().notLe(a, b); }
  static F blend(F a, F b, F mask) { return v().select(mask, b, a); }  // mask ? b : a
};

class IrScope {
public:
  explicit IrScope(Vx& v) noexcept : prev_(IrS::cur()) { IrS::cur() = &v; }
  ~IrScope() { IrS::cur() = prev_; }
  IrScope(const IrScope&) = delete;
  IrScope& operator=(const IrScope&) = delete;

private:
  Vx* prev_;
};

}  // namespace pq::jit::detail
