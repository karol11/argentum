#ifndef AK_UTF8_H__
#define AK_UTF8_H__

#ifdef __cplusplus
extern "C" {
#endif

int get_utf8(const char** ptr);
int put_utf8(int v, void* ctx, int(*put_fn)(void*, int));

#ifdef __cplusplus
}  // extern "C"
#endif

#endif // AK_UTF8_H__
