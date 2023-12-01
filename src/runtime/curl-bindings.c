#include <assert.h>
#include "runtime.h"
#include "map/map-base.h"
#include "ag-threads.h"
#include "ag-queue.h"
#include "../../curl/include/curl/curl.h"

typedef struct AgHttpRequest {
	AgObject header;
	AgString* url;
	AgString* verb;
	AgBlob* body;
	AgBlob* headers;
} AgHttpRequest;

typedef struct AgHttpResponse {
	AgObject header;
	AgHttpRequest* request;
	uint64_t status;
	AgMap* headers;
	AgBlob* body;
} AgHttpResponse;

typedef struct ag_http_task {
	struct ag_http_task* prev;
	struct ag_http_task* next;
	AgHttpResponse* reponse;
	AgWeak* on_end_data;
	void* on_end_proc;
	CURL* easy;
	struct curl_slist* headers;
} ag_http_task;

pthread_mutex_t ag_http_mutex;
pthread_cond_t ag_http_cvar;
ag_queue ag_http_queue;
pthread_t ag_http_thread;
CURLM* ag_curl_multi = NULL;

static size_t header_callback(char* buffer, size_t isize, size_t nitems, void* userdata) {
	if (((ag_http_task*)userdata)->on_end_data->target == NULL)
		return 0;  // raise CURLE_WRITE_ERROR
	size_t size = nitems * isize;
	char* delim = memchr(buffer, size, ':');
	if (delim) {
		ag_m_sys_SharedMap_setAt(
			((ag_http_task*)userdata)->reponse->headers,
			ag_make_str(buffer, delim - buffer),
			ag_make_str(delim + 1, size - (delim - buffer) - 1));
	}
	return size;
}

static size_t body_callback(char* ptr, size_t isize, size_t nmemb, void* userdata) {
	if (((ag_http_task*)userdata)->on_end_data->target == NULL)
		return 0;  // raise CURLE_WRITE_ERROR
	size_t size = nmemb * isize;
	AgBlob* body = ((ag_http_task*)userdata)->reponse->body;
	size_t at = body->size;
	ag_make_blob_fit(body, at + size);
	ag_memcpy(body->data + at, ptr, size);
	return size;
}

static void delete_task(ag_http_task* task) {
	task->next->prev = task->prev;
	task->prev->next = task->next;
	curl_multi_remove_handle(ag_curl_multi, task->easy);
	curl_easy_cleanup(task->easy);
	if (task->headers)
		curl_slist_free_all(task->headers);
	ag_release_own(&task->reponse->header);
	ag_release_weak(task->on_end_data);
	ag_free(task);
}

static void create_task(
	ag_http_task* root_task,
	AgHttpResponse* resp,
	AgWeak* on_end_data,
	void* on_end_proc)
{
	ag_http_task* task = ag_alloc(sizeof(ag_http_task));
	task->next = root_task->next;
	task->prev = root_task;
	task->next->prev = task;
	task->prev->next = task;
	task->reponse = resp;
	task->on_end_data = on_end_data;
	task->on_end_proc = on_end_proc;
	task->easy = curl_easy_init();
	task->headers = NULL;
	AgHttpRequest* req = task->reponse->request;
	curl_easy_setopt(task->easy, CURLOPT_URL, req->url->ptr);
	curl_easy_setopt(task->easy, CURLOPT_CUSTOMREQUEST, req->verb->ptr);
	if (req->body->size) {
		curl_easy_setopt(task->easy, CURLOPT_POSTFIELDS, req->body->data);
		curl_easy_setopt(task->easy, CURLOPT_POSTFIELDSIZE, req->body->size);
	}
	for (AgString* i = (AgString*)req->headers->data, *e = i + req->headers->size; i < e; ++i)
		task->headers = curl_slist_append(task->headers, i->ptr);
	if (task->headers)
		curl_easy_setopt(task->easy, CURLOPT_HTTPHEADER, task->headers);
	curl_easy_setopt(task->easy, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
	curl_easy_setopt(task->easy, CURLOPT_HEADERFUNCTION, header_callback);
	curl_easy_setopt(task->easy, CURLOPT_HEADERDATA, task);
	curl_easy_setopt(task->easy, CURLOPT_WRITEFUNCTION, body_callback);
	curl_easy_setopt(task->easy, CURLOPT_WRITEDATA, task);
	curl_easy_setopt(task->easy, CURLOPT_PRIVATE, task);
	curl_multi_add_handle(ag_curl_multi, task->easy);
}

void trampoline(AgObject* self, ag_fn entry_point, ag_thread* th) {
	AgObject* response = (AgObject*)ag_get_thread_param(th);
	ag_unlock_thread_queue(th);
	((void (*)(AgObject*, AgObject*))entry_point)(self, response);
	ag_release_own_nn(response);
}

void* http_thread_proc(void* unused) {
	ag_http_task root_task;
	root_task.next = root_task.prev = &root_task;
	for (;;) {
		pthread_mutex_lock(&ag_http_mutex);
		while (ag_http_queue.read_pos != ag_http_queue.write_pos) {
			uint64_t resp = ag_read_queue(&ag_http_queue);
			if (resp == 0)
				goto terminate;
			uint64_t on_end_data = ag_read_queue(&ag_http_queue);
			uint64_t on_end_proc = ag_read_queue(&ag_http_queue);
			pthread_mutex_unlock(&ag_http_mutex);
			create_task(&root_task, (AgHttpResponse*)resp, (AgWeak*)on_end_data, (void*)on_end_proc);
			pthread_mutex_lock(&ag_http_mutex);
		}
		if (root_task.next == &root_task) {
			pthread_cond_wait(&ag_http_cvar, &ag_http_mutex);
		} else {
			pthread_mutex_unlock(&ag_http_mutex);
			int count;
			curl_multi_perform(ag_curl_multi, &count);
			struct CURLMsg* m;
			while (m = curl_multi_info_read(ag_curl_multi, &count)) {
				assert(m->msg == CURLMSG_DONE);
				ag_http_task* task = NULL;
				curl_easy_getinfo(m->easy_handle, CURLINFO_PRIVATE, &task);
				long code;
				curl_easy_getinfo(m->easy_handle, CURLINFO_RESPONSE_CODE, &code);
				task->reponse->status = code;
				ag_thread* th = ag_prepare_post(task->on_end_data, trampoline, task->on_end_proc, 1);
				if (th) {
					ag_post_own_param(th, &task->reponse->header);
					ag_finalize_post(th);
					task->reponse = NULL;
					task->on_end_data = NULL;
				}
				delete_task(task);
			}
			curl_multi_poll(ag_curl_multi, NULL, 0, 0, &count);
		}
	}
terminate:
	pthread_mutex_unlock(&ag_http_mutex);
	while (root_task.next != &root_task)
		delete_task(root_task.next);
	return NULL;
}

void* ag_m_httpClient_HttpClient_httpClient_start(void* cl) {
	assert(ag_curl_multi == NULL);
	pthread_mutex_init(&ag_http_mutex, NULL);
	pthread_cond_init(&ag_http_cvar, NULL);
	ag_init_queue(&ag_http_queue);
	ag_curl_multi = curl_multi_init();
	pthread_create(&ag_http_thread, NULL, http_thread_proc, NULL);
	return cl;
}

void ag_fn_httpClient_executeRequestInternal(AgHttpResponse* response, AgWeak* on_end_data, void* on_end_proc) {
	assert(ag_curl_multi != NULL);
	ag_detach_own(&response->header);
	ag_detach_weak(on_end_data);
	pthread_mutex_lock(&ag_http_mutex);
	ag_resize_queue(&ag_http_queue, 3);
	ag_write_queue(&ag_http_queue, (uint64_t)response);
	ag_write_queue(&ag_http_queue, (uint64_t)on_end_data);
	ag_write_queue(&ag_http_queue, (uint64_t)on_end_proc);
	pthread_mutex_unlock(&ag_http_mutex);
	curl_multi_wakeup(&ag_curl_multi);
	pthread_cond_broadcast(&ag_http_cvar);
}

void ag_fn_httpClient_destroyHttpClient(void* unused) {
	assert(ag_curl_multi != NULL);
	pthread_mutex_lock(&ag_http_mutex);
	ag_resize_queue(&ag_http_queue, 1);
	ag_write_queue(&ag_http_queue, 0);
	pthread_mutex_unlock(&ag_http_mutex);
	curl_multi_wakeup(&ag_curl_multi);
	pthread_cond_broadcast(&ag_http_cvar);
	void* unused_res;
	pthread_join(&ag_http_thread, &unused_res);
	pthread_mutex_destroy(&ag_http_mutex);
	pthread_cond_destroy(&ag_http_cvar);
	curl_multi_cleanup(ag_curl_multi);
	ag_curl_multi = NULL;
}
