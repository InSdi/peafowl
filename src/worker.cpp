/*
 * worker.cpp
 *
 * Created on: 12/11/2012
 * =========================================================================
 *  Copyright (C) 2012-2013, Daniele De Sensi (d.desensi.software@gmail.com)
 *
 *  This file is part of Peafowl.
 *
 *  Peafowl is free software: you can redistribute it and/or
 *  modify it under the terms of the Lesser GNU General Public
 *  License as published by the Free Software Foundation, either
 *  version 3 of the License, or (at your option) any later version.

 *  Peafowl is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  Lesser GNU General Public License for more details.
 *
 *  You should have received a copy of the Lesser GNU General Public
 *  License along with Peafowl.
 *  If not, see <http://www.gnu.org/licenses/>.
 *
 * ====================================================================
 */

#include "worker.hpp"
#include "mc_dpi_api.h"
#include "flow_table.h"
#include <pthread.h>
#include <stdlib.h>
#include <ff/mapping_utils.hpp>

#if DPI_NUMA_AWARE
#include <numa.h>
#endif

namespace dpi{

#ifndef DPI_DEBUG
static inline
#endif
mc_dpi_task_t* dpi_allocate_task(){
	mc_dpi_task_t* r;
#if DPI_NUMA_AWARE
	r=(mc_dpi_task_t*) numa_alloc_onnode(sizeof(mc_dpi_task_t),
			           DPI_NUMA_AWARE_TASKS_NODE);
#else
	#if DPI_MULTIPROCESSOR_ALIGN_TASKS
		assert(posix_memalign((void**) &r, DPI_CACHE_LINE_SIZE,
			   sizeof(mc_dpi_task_t))==0);
	#else
		r=new mc_dpi_task_t;
	#endif
#endif
	assert(r);
	return r;
}

#ifndef DPI_DEBUG
static inline
#endif
void dpi_free_task(mc_dpi_task_t* task){
#if DPI_NUMA_AWARE
	numa_free(task, sizeof(mc_dpi_task_t));
#else
	#if DPI_MULTIPROCESSOR_ALIGN_TASKS
		free(task);
	#else
		delete task;
	#endif
#endif
}




/*****************************************************/
/*                      L3_L4 nodes.                 */
/*****************************************************/
dpi_L3_L4_emitter::dpi_L3_L4_emitter(mc_dpi_packet_reading_callback** cb,
		                             void** user_data,
		                             u_int8_t* freeze_flag,
		                             u_int8_t* terminating,
		                             u_int16_t proc_id,
		                             ff::SWSR_Ptr_Buffer* tasks_pool)
                                     :cb(cb), user_data(user_data),
                                      freeze_flag(freeze_flag),
                                      terminating(terminating),
                                      proc_id(proc_id),
                                      tasks_pool(tasks_pool),
                                      initialized(0){
	;
}

int dpi_L3_L4_emitter::svc_init(){
	worker_debug_print("[worker.cpp]: L3_L4 emitter mapped on "
			           "processor: %d\n", proc_id);
	ff_mapThreadToCpu(proc_id,-20);

	if(!initialized){
		/** Fill the task pool. **/
#if DPI_MULTIPROCESSOR_USE_TASKS_POOL
		for(uint i=0; i<DPI_MULTIPROCESSOR_TASKS_POOL_SIZE; i++){
			tasks_pool->push(dpi_allocate_task());
		}
#endif
		initialized=1;
	}
	return 0;
}

void* dpi_L3_L4_emitter::svc(void* task){
	mc_dpi_packet_reading_result_t packet;
	mc_dpi_task_t* r=NULL;

	if(unlikely(*freeze_flag)){
		worker_debug_print("%s\n", "[worker.cpp]: Freeze message received,"
				           " terminating.");
		return (void*) ff::FF_EOS;
	}

#if DPI_MULTIPROCESSOR_USE_TASKS_POOL
	if(!tasks_pool->empty()){
		tasks_pool->pop((void**) &r);
	}else{
		r=dpi_allocate_task();
	}
#else
	r=dpi_allocate_task();
#endif

	for(uint i=0; i<DPI_MULTIPROCESSOR_DEFAULT_GRAIN_SIZE; i++){
		packet=(*(*cb))(*user_data);
		if(unlikely(packet.pkt==NULL)){
			worker_debug_print("%s\n", "[worker.cpp]: No more task to "
					           "process, terminating.");
			*terminating=1;
			return (void*) ff::FF_EOS;
		}

		r->input_output_task_t.L3_L4_input_task_t[i].user_pointer=
				packet.user_pointer;
		r->input_output_task_t.L3_L4_input_task_t[i].current_time=
				packet.current_time;
		r->input_output_task_t.L3_L4_input_task_t[i].length=
				packet.length;
		r->input_output_task_t.L3_L4_input_task_t[i].pkt=
				packet.pkt;
        __builtin_prefetch(
        		&(r->input_output_task_t.L3_L4_input_task_t[i+5]), 1, 0);
	}
	return (void*) r;
}

dpi_L3_L4_emitter::~dpi_L3_L4_emitter(){
	;
}


dpi_L3_L4_worker::dpi_L3_L4_worker(dpi_library_state_t* state,
		                           uint worker_id,
		                           u_int16_t proc_id,
		                           u_int32_t v4_worker_table_size,
		                           u_int32_t v6_worker_table_size)
                                  :state(state),
                                   v4_worker_table_size(v4_worker_table_size),
                                   v6_worker_table_size(v6_worker_table_size),
                                   worker_id(worker_id),
                                   proc_id(proc_id){
	assert(posix_memalign((void**) &in, DPI_CACHE_LINE_SIZE,
		   sizeof(L3_L4_input_task_struct)*
		   DPI_MULTIPROCESSOR_DEFAULT_GRAIN_SIZE)==0);
}

dpi_L3_L4_worker::~dpi_L3_L4_worker(){
	free(in);
}

int dpi_L3_L4_worker::svc_init(){
	worker_debug_print("[worker.cpp]: L3_L4 worker %d mapped "
		               "on processor: %d\n", worker_id, proc_id);
	ff_mapThreadToCpu(proc_id,-20);
	return 0;
}


void* dpi_L3_L4_worker::svc(void* task){
	mc_dpi_task_t* real_task=(mc_dpi_task_t*) task;

	/**
	 * Here we need a copy. Indeed, the task is a union and, if
	 * we do not make the copy, we overwrite the input tasks with
	 * the generated output tasks.
	 **/
	memcpy(in, real_task->input_output_task_t.L3_L4_input_task_t,
			DPI_MULTIPROCESSOR_DEFAULT_GRAIN_SIZE*
			sizeof(L3_L4_input_task_struct));

	dpi_pkt_infos_t pkt_infos;
	for(uint i=0; i<DPI_MULTIPROCESSOR_DEFAULT_GRAIN_SIZE; i++){
        __builtin_prefetch(&(in[i+2]), 0, 0);
        __builtin_prefetch((in[i+2]).pkt, 0, 0);

		real_task->input_output_task_t.L3_L4_output_task_t[i].status=
				mc_dpi_extract_packet_infos(this->state, in[i].pkt,
						                    in[i].length, &pkt_infos,
						                    in[i].current_time,
						                    worker_id);

		/* To have always a consistent value temp L7 worker selection. */
		real_task->input_output_task_t.
			L3_L4_output_task_t[i].hash_result=0;
		real_task->input_output_task_t.
			L3_L4_output_task_t[i].destination_worker=0;
		real_task->input_output_task_t.
			L3_L4_output_task_t[i].user_pointer=in[i].user_pointer;

		if(likely(real_task->input_output_task_t.
					L3_L4_output_task_t[i].status>=0)){

			if(pkt_infos.l4prot!=IPPROTO_TCP &&
			   pkt_infos.l4prot!=IPPROTO_UDP){
				real_task->input_output_task_t.
					L3_L4_output_task_t[i].status=
							DPI_ERROR_TRANSPORT_PROTOCOL_NOTSUPPORTED;
				continue;
			}

			if(likely(real_task->input_output_task_t
						.L3_L4_output_task_t[i].status!=
								DPI_STATUS_IP_FRAGMENT)){
				real_task->input_output_task_t.
						L3_L4_output_task_t[i].pkt_infos=pkt_infos;
				if(pkt_infos.ip_version==DPI_IP_VERSION_4){
					real_task->input_output_task_t.L3_L4_output_task_t[i].
						hash_result=dpi_compute_v4_hash_function(
								(dpi_flow_DB_v4*) this->state->db4,
								&pkt_infos);
					real_task->input_output_task_t.L3_L4_output_task_t[i].
						destination_worker=real_task->input_output_task_t.
						L3_L4_output_task_t[i].
						hash_result/v4_worker_table_size;
				}else{
					real_task->input_output_task_t.L3_L4_output_task_t[i].
					hash_result=dpi_compute_v6_hash_function(
						(dpi_flow_DB_v6*) this->state->db6, &pkt_infos);
					real_task->input_output_task_t.L3_L4_output_task_t[i].
					destination_worker=real_task->input_output_task_t.
					L3_L4_output_task_t[i].
					hash_result/v6_worker_table_size;
				}
			}
		}
	}
	return real_task;
}


dpi_L3_L4_collector::dpi_L3_L4_collector(u_int16_t proc_id):
		proc_id(proc_id){
	;
}

int dpi_L3_L4_collector::svc_init(){
	worker_debug_print("[worker.cpp]: L3_L4 collector mapped "
			           "on processor: %d\n", proc_id);
	ff_mapThreadToCpu(proc_id,-20);
	return 0;
}

void* dpi_L3_L4_collector::svc(void* task){
	return task;
}


/*******************************************************************/
/*                          L7 nodes.                              */
/*******************************************************************/

dpi_L7_emitter::dpi_L7_emitter(dpi_L7_scheduler* lb,
		                       u_int16_t num_L7_workers,
		                       u_int16_t proc_id)
                              :lb(lb), proc_id(proc_id){
	assert(posix_memalign((void**) &partially_filled_sizes,
		  DPI_CACHE_LINE_SIZE, (sizeof(uint)*num_L7_workers)+
		  DPI_CACHE_LINE_SIZE)==0);
	bzero(partially_filled_sizes, sizeof(uint)*num_L7_workers);

	assert(posix_memalign((void**) &partially_filled,
		  DPI_CACHE_LINE_SIZE,
		  (sizeof(mc_dpi_task_t)*num_L7_workers)+
		  DPI_CACHE_LINE_SIZE)==0);
	bzero(partially_filled, sizeof(mc_dpi_task_t)*num_L7_workers);

	assert(posix_memalign((void**) &waiting_tasks,
		   DPI_CACHE_LINE_SIZE,
		   (sizeof(mc_dpi_task_t*)*num_L7_workers*2)+
		   DPI_CACHE_LINE_SIZE)==0);

	waiting_tasks_size=0;
	for(uint i=0; i<num_L7_workers; i++){
		waiting_tasks[i]=dpi_allocate_task();
		++waiting_tasks_size;
	}
}

dpi_L7_emitter::~dpi_L7_emitter(){
	free(partially_filled_sizes);
	free(partially_filled);
}

int dpi_L7_emitter::svc_init(){
	worker_debug_print("[worker.cpp]: L7 emitter mapped "
			           "on processor: %d\n", proc_id);
	ff_mapThreadToCpu(proc_id,-20);
	return 0;
}

void* dpi_L7_emitter::svc(void* task){
	mc_dpi_task_t* real_task=(mc_dpi_task_t*) task;
	u_int16_t destination_worker;
	mc_dpi_task_t* out;
	uint pfs;

	for(uint i=0; i<DPI_MULTIPROCESSOR_DEFAULT_GRAIN_SIZE; i++){
        __builtin_prefetch(&(real_task->input_output_task_t.
        		L3_L4_output_task_t[i+4]), 0, 0);
		destination_worker=real_task->input_output_task_t.
				L3_L4_output_task_t[i].destination_worker;
		worker_debug_print("[worker.cpp]: L7 emitter: Inserted"
				           " a task into the queue of worker: "
				           "%d\n", destination_worker);
		pfs=partially_filled_sizes[destination_worker];
        __builtin_prefetch(&(partially_filled[destination_worker].
        		input_output_task_t.L3_L4_output_task_t[pfs]), 1, 0);

		if(pfs+1==DPI_MULTIPROCESSOR_DEFAULT_GRAIN_SIZE){
			assert(waiting_tasks_size!=0);
			out=waiting_tasks[--waiting_tasks_size];
			memcpy(out->input_output_task_t.L3_L4_output_task_t,
				   partially_filled[destination_worker].
				   	   input_output_task_t.L3_L4_output_task_t,
				   sizeof(L3_L4_output_task_struct)*
				   	   (DPI_MULTIPROCESSOR_DEFAULT_GRAIN_SIZE-1));
			out->input_output_task_t.L3_L4_output_task_t
				[DPI_MULTIPROCESSOR_DEFAULT_GRAIN_SIZE-1]=
						real_task->input_output_task_t.
							L3_L4_output_task_t[i];
			lb->set_victim(destination_worker);
			while(ff_send_out((void*) out)==false);
			partially_filled_sizes[destination_worker]=0;
		}else{
			partially_filled[destination_worker].input_output_task_t.
				L3_L4_output_task_t[pfs]=real_task->input_output_task_t.
					L3_L4_output_task_t[i];
			++partially_filled_sizes[destination_worker];
		}
	}
	waiting_tasks[waiting_tasks_size]=real_task;
	++waiting_tasks_size;
	return (void*) ff::FF_GO_ON;
}



dpi_L7_worker::dpi_L7_worker(dpi_library_state_t* state,
	                         u_int16_t worker_id,
	                         u_int16_t proc_id):
	                         state(state),
	                         worker_id(worker_id),
	                         proc_id(proc_id){
	assert(posix_memalign((void**) &this->temp, DPI_CACHE_LINE_SIZE,
				          (sizeof(L3_L4_output_task_struct)*
				        		  DPI_MULTIPROCESSOR_DEFAULT_GRAIN_SIZE)+
				          DPI_CACHE_LINE_SIZE)==0);
}

dpi_L7_worker::~dpi_L7_worker(){
	free(temp);
}

int dpi_L7_worker::svc_init(){
	worker_debug_print("[worker.cpp]: L7 worker %d mapped on"
			           " processor: %d\n", worker_id, proc_id);
	ff_mapThreadToCpu(proc_id,-20);
	return 0;
}


void* dpi_L7_worker::svc(void* task){
	mc_dpi_task_t* real_task=(mc_dpi_task_t*) task;
	int8_t l3_status;
	dpi_pkt_infos_t infos;
	dpi_flow_infos_t* flow_infos;
	ipv4_flow_t* ipv4_flow;
	ipv6_flow_t* ipv6_flow;

	memcpy(temp, real_task->input_output_task_t.L3_L4_output_task_t,
		   DPI_MULTIPROCESSOR_DEFAULT_GRAIN_SIZE*
		   	   sizeof(L3_L4_output_task_struct));

	for(uint i=0; i<DPI_MULTIPROCESSOR_DEFAULT_GRAIN_SIZE; i++){
		real_task->input_output_task_t.L7_output_task_t[i].user_pointer=
				temp[i].user_pointer;
		ipv4_flow=NULL;
		ipv6_flow=NULL;

		l3_status=temp[i].status;
		if(unlikely(l3_status<0 || l3_status==DPI_STATUS_IP_FRAGMENT)){
			real_task->input_output_task_t.L7_output_task_t[i].
				result.status=l3_status;
			continue;
		}
		infos=temp[i].pkt_infos;
        __builtin_prefetch(temp[i+1].pkt_infos.pkt+temp[i+1].
        		pkt_infos.l7offset, 0, 0);

		if(infos.ip_version==DPI_IP_VERSION_4){
			ipv4_flow=mc_dpi_flow_table_find_or_create_flow_v4(
					state, this->worker_id, temp[i].hash_result,
					&(infos));
			flow_infos=&(ipv4_flow->infos);
		}else{
			ipv6_flow=mc_dpi_flow_table_find_or_create_flow_v6(
					state, this->worker_id, temp[i].hash_result,
					&(infos));
			flow_infos=&(ipv6_flow->infos);
		}

		real_task->input_output_task_t.L7_output_task_t[i].result.
			status=DPI_STATUS_OK;
		if(unlikely(flow_infos==NULL)){
			real_task->input_output_task_t.L7_output_task_t[i].result.
				status=DPI_ERROR_MAX_FLOWS;
			if(unlikely(l3_status==DPI_STATUS_IP_LAST_FRAGMENT)){
				free((unsigned char*) infos.pkt);
			}
			break;
		}

		real_task->input_output_task_t.L7_output_task_t[i].result=
				dpi_stateless_get_app_protocol(state, flow_infos,
						                       &(infos));
		if(real_task->input_output_task_t.L7_output_task_t[i].result.
				status==DPI_STATUS_TCP_CONNECTION_TERMINATED){
			if(ipv4_flow!=NULL){
				mc_dpi_flow_table_delete_flow_v4(
						(dpi_flow_DB_v4_t*) state->db4,
						state->flow_cleaner_callback,
						this->worker_id,
						ipv4_flow);
			}else{
				mc_dpi_flow_table_delete_flow_v6(
						(dpi_flow_DB_v6_t*) state->db6,
						state->flow_cleaner_callback,
						this->worker_id,
						ipv6_flow);
			}
		}

		if(unlikely(l3_status==DPI_STATUS_IP_LAST_FRAGMENT)){
			free((unsigned char*) infos.pkt);
		}
	}
	return real_task;
}


dpi_L7_collector::dpi_L7_collector(mc_dpi_processing_result_callback** cb,
		                           void** user_data,
		                           u_int16_t proc_id,
		                           ff::SWSR_Ptr_Buffer* tasks_pool)
                                   :cb(cb), user_data(user_data),
                                    proc_id(proc_id),
                                    tasks_pool(tasks_pool),
                                    initialized(0){
	;
}

int dpi_L7_collector::svc_init(){
	worker_debug_print("[worker.cpp]: L7 collector"
			          " mapped on processor: %d\n", proc_id);
	ff_mapThreadToCpu(proc_id,-20);
	if(!initialized){
		initialized=1;
	}
	return 0;
}

void* dpi_L7_collector::svc(void* task){
	mc_dpi_processing_result_t r;
	mc_dpi_task_t* real_task=(mc_dpi_task_t*) task;
	for(uint i=0; i<DPI_MULTIPROCESSOR_DEFAULT_GRAIN_SIZE; i++){
		r.result=real_task->input_output_task_t.L7_output_task_t[i].
				result;
		r.user_pointer=real_task->input_output_task_t.
				L7_output_task_t[i].user_pointer;
		(*(*cb))(&r, *user_data);
	}
#if DPI_MULTIPROCESSOR_USE_TASKS_POOL
	if(tasks_pool->available()){
		tasks_pool->push(task);
	}else{
		dpi_free_task(real_task);
	}
#else
	dpi_free_task(real_task, allocator);
#endif
	return (void*) ff::FF_GO_ON;
}

void dpi_L7_collector::svc_end(){
#if DPI_MULTIPROCESSOR_USE_TASKS_POOL
	mc_dpi_task_t* task=NULL;
	while(!tasks_pool->empty()){
		tasks_pool->pop((void**) &task);
		dpi_free_task(task);
	}
#endif
}


dpi_collapsed_emitter::dpi_collapsed_emitter(
		mc_dpi_packet_reading_callback** cb,
		void** user_data, u_int8_t* freeze_flag,
		u_int8_t* terminating,
		ff::SWSR_Ptr_Buffer* tasks_pool,
		dpi_library_state_t* state,
		u_int32_t v4_worker_table_size,
		u_int32_t v6_worker_table_size,
		dpi_L7_scheduler* lb,
		u_int16_t num_workers, u_int16_t proc_id):
				dpi_L7_emitter(lb, num_workers, proc_id),
				proc_id(proc_id){
	L3_L4_emitter=new dpi::dpi_L3_L4_emitter(cb, user_data,
			                                 freeze_flag,
			                                 terminating,
			                                 proc_id,
			                                 tasks_pool);
	L3_L4_worker=new dpi::dpi_L3_L4_worker(state, 0, proc_id,
			                               v4_worker_table_size,
			                               v6_worker_table_size);
}

dpi_collapsed_emitter::~dpi_collapsed_emitter(){
	delete L3_L4_emitter;
	delete L3_L4_worker;
}

int dpi_collapsed_emitter::svc_init(){
	L3_L4_emitter->svc_init();
	L3_L4_worker->svc_init();
	dpi_L7_emitter::svc_init();
	worker_debug_print("[worker.cpp]: collapsed emitter mapped "
			           "on processor: %d\n", proc_id);
	return 0;
}
void* dpi_collapsed_emitter::svc(void* task){
	void* r=L3_L4_emitter->svc(task);
	if(unlikely(r==(void*) ff::FF_EOS)){
		return r;
	}else{
		return dpi_L7_emitter::svc(L3_L4_worker->svc(r));
	}
}

void dpi_collapsed_emitter::svc_end(){
	L3_L4_emitter->svc_end();
	L3_L4_worker->svc_end();
	dpi_L7_emitter::svc_end();
}


}
