#ifndef CHIPYARD_NATIVE_REALTIME_EXPORTS_API_H
#define CHIPYARD_NATIVE_REALTIME_EXPORTS_API_H
#ifdef __cplusplus
extern "C" {
#endif
double rt_identity(double);
double rt_import_identity(double);
double rt_c_identity(double);
double rt_saved(void);
double rt_zero(void);
double rt_constant(void);
double rt_transform(double, double *, double *);
double rt_now(void);
double rt_direct_time(void);
double rt_literal(void);
#ifdef __cplusplus
}
#endif
#endif
