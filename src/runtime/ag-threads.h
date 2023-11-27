#include <time.h>  // timespec, timespec_get

static inline uint64_t timespec_to_ms(const struct timespec* time) {
	return time->tv_nsec / 1000000 + time->tv_sec * 1000;
}

typedef void* (*ag_thread_start_t) (void*);

#ifdef WIN32

#include <windows.h>

// Thread
typedef HANDLE pthread_t;
static inline int pthread_create(pthread_t* thr, void* unused_attr, ag_thread_start_t func, void* arg) {
	HANDLE r = CreateThread(
		NULL,                   // default security attributes
		0,                      // use default stack size  
		(LPTHREAD_START_ROUTINE)func,
		arg,                    // argument to thread function 
		0,                      // use default creation flags 
		NULL
	);
	if (r == NULL)
		return -1;
	*thr = r;
	return 0;
}
static inline int pthread_join(pthread_t thr, void** usused_res) {
	return WaitForSingleObject(thr, INFINITE)
		? -1
		: 0;  // success if 0 (WAIT_OBJECT_0)
}

// Mutex
typedef CRITICAL_SECTION pthread_mutex_t;
static inline int pthread_mutex_init(pthread_mutex_t* mutex, const void* unused_attr) {
	return InitializeCriticalSectionAndSpinCount(mutex, 0x00000400)
		? 0
		: -1;
}

#define pthread_mutex_destroy DeleteCriticalSection

static inline int pthread_mutex_lock(pthread_mutex_t* mutex) {
	EnterCriticalSection(mutex);
	return 0;
}

static inline int pthread_mutex_unlock(pthread_mutex_t* mutex) {
	LeaveCriticalSection(mutex);
	return 0;
}

// CVar
typedef CONDITION_VARIABLE pthread_cond_t;

static inline int pthread_cond_init(pthread_cond_t* cond, const void* unused_attr) {
	InitializeConditionVariable(cond);
	return 0;
}

static inline int pthread_cond_destroy(pthread_cond_t* cond) {
	// do nothing
	return 0;
}

static inline int pthread_cond_broadcast(pthread_cond_t* cond) {
	WakeAllConditionVariable(cond);
	return 0;
}

static inline int pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* timeout) {
	struct timespec now;
	return SleepConditionVariableCS(
		cond,
		mutex,
		timespec_get(&now, TIME_UTC)
		? (DWORD)(timespec_to_ms(timeout) - timespec_to_ms(&now))
		: INFINITE
	) ? 0 : -1;
}

static inline int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
	return SleepConditionVariableCS(cond, mutex, INFINITE) ? 0 : -1;
}

#define AG_THREAD_LOCAL __declspec(thread)

#else

#include <pthread.h>

#define AG_THREAD_LOCAL _Thread_local

#endif
