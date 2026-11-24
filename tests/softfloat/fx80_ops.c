// Test entry points for millicode/softfloat/softfloat.h, one per operation.
// Inputs: a = D0 (sign/exponent), D1:D2 (significand); b = D3, D4:D5;
// D6 = rounding mode (SoftFloat's float_round_*), D7 = rounding precision
// (32, 64 or 80). Outputs: D0, D1:D2 = the floatx80 result (or D0 = the
// int32 result), D3 = SoftFloat's exception flags. run_fx80.cc runs these on
// the uisa ISS and compares with Previous's SoftFloat.
#include "../../millicode/softfloat/softfloat.h"

inline floatx80 argA() {
  floatx80 a;
  a.high = __rdg(D0);
  a.low = ((uint64_t)__rdg(D1) << 32) | __rdg(D2);
  return a;
}
inline floatx80 argB() {
  floatx80 b;
  b.high = __rdg(D3);
  b.low = ((uint64_t)__rdg(D4) << 32) | __rdg(D5);
  return b;
}
inline void initStatus(float_status &s) {
  s.float_exception_flags = 0;
  s.float_rounding_mode = __rdg(D6);
  s.floatx80_rounding_precision = __rdg(D7);
}
inline void done(floatx80 z, float_status &s) {
  __wrg(D0, z.high);
  __wrg(D1, (uint32_t)(z.low >> 32));
  __wrg(D2, (uint32_t)z.low);
  __wrg(D3, s.float_exception_flags);
  __wrg(GPC, __next_pc());
  __gexit();
}
inline void doneInt(int32_t z, float_status &s) {
  __wrg(D0, z);
  __wrg(D3, s.float_exception_flags);
  __wrg(GPC, __next_pc());
  __gexit();
}

entry void fx80_add(void) { float_status s; initStatus(s); done(floatx80_add(argA(), argB(), s), s); }
entry void fx80_sub(void) { float_status s; initStatus(s); done(floatx80_sub(argA(), argB(), s), s); }
entry void fx80_mul(void) { float_status s; initStatus(s); done(floatx80_mul(argA(), argB(), s), s); }
entry void fx80_div(void) { float_status s; initStatus(s); done(floatx80_div(argA(), argB(), s), s); }
entry void fx80_sqrt(void) { float_status s; initStatus(s); done(floatx80_sqrt(argA(), s), s); }
entry void fx80_move(void) { float_status s; initStatus(s); done(floatx80_move(argA(), s), s); }
entry void fx80_from_i32(void) { float_status s; initStatus(s); done(int32_to_floatx80((int32_t)__rdg(D0)), s); }
entry void fx80_to_i32(void) { float_status s; initStatus(s); doneInt(floatx80_to_int32(argA(), s), s); }
entry void fx80_sglmul(void) { float_status s; initStatus(s); done(floatx80_sglmul(argA(), argB(), s), s); }
entry void fx80_sgldiv(void) { float_status s; initStatus(s); done(floatx80_sgldiv(argA(), argB(), s), s); }
entry void fx80_abs(void) { float_status s; initStatus(s); done(floatx80_abs(argA(), s), s); }
entry void fx80_neg(void) { float_status s; initStatus(s); done(floatx80_neg(argA(), s), s); }
entry void fx80_cmp(void) { float_status s; initStatus(s); done(floatx80_cmp(argA(), argB(), s), s); }
entry void fx80_tst(void) { float_status s; initStatus(s); done(floatx80_tst(argA(), s), s); }
