/**
* Equihash solver interface for ccminer (compatible with linux and windows)
* Solver taken from nheqminer, by djeZo (and NiceHash)
* tpruvot - 2017 (GPL v3)
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#define VERUS_KEY_SIZE 8832
#define VERUS_KEY_SIZE128 552
#include <stdexcept>
#include <vector>
#include "verus_hash.h"
#include "verus_clhash.h"
#include "uint256.h"
//#include "hash.h"
#include <miner.h>
//#include "primitives/block.h"
//extern "C"
//{
//#include "haraka.h"

//}
enum
{
	// primary actions
	SER_NETWORK = (1 << 0),
	SER_DISK = (1 << 1),
	SER_GETHASH = (1 << 2),
};
// input here is 140 for the header and 1344 for the solution (equi.cpp)
static const int PROTOCOL_VERSION = 170002;

#include <cuda_helper.h>

#define EQNONCE_OFFSET 30 /* 27:34 */
#define NONCE_OFT EQNONCE_OFFSET

static bool init[MAX_GPUS] = { 0 };
static u128 data_key[MAX_GPUS][VERUS_KEY_SIZE128] = { 0 }; // 552 required
static __thread uint32_t throughput = 0;
extern void verus_hash(int thr_id, uint32_t threads, uint32_t startNonce, uint32_t* resNonces);
extern void verus_setBlock(uint8_t *blockf, uint32_t *pTargetIn, uint8_t *lkey, int thr_id);
extern void verus_init(int thr_id);


#ifndef htobe32
#define htobe32(x) swab32(x)
#endif

extern "C" void GenNewCLKey(unsigned char *seedBytes32, u128 *keyback)
{
		// generate a new key by chain hashing with Haraka256 from the last curbuf
		int n256blks = VERUS_KEY_SIZE >> 5;  //8832 >> 5
		int nbytesExtra = VERUS_KEY_SIZE & 0x1f;  //8832 & 0x1f
		unsigned char *pkey = (unsigned char*)keyback;
		unsigned char *psrc = seedBytes32;
		for (int i = 0; i < n256blks; i++)
		{
			haraka256_port(pkey, psrc);

			psrc = pkey;
			pkey += 32;
		}
		if (nbytesExtra)
		{
			unsigned char buf[32];
			haraka256_port(buf, psrc);
			memcpy(pkey, buf, nbytesExtra);
		}
}



extern "C" void VerusHashHalf(void *result2, unsigned char *data, size_t len)
{
	alignas(32) unsigned char buf1[64] = { 0 }, buf2[64];
	unsigned char *curBuf = buf1, *result = buf2;
	size_t curPos = 0;
	//unsigned char result[64];
	curBuf = buf1;
	result = buf2;
	curPos = 0;
	std::fill(buf1, buf1 + sizeof(buf1), 0);

	unsigned char *tmp;

	load_constants_port();

	// digest up to 32 bytes at a time
	for (int pos = 0; pos < len; )
	{
		int room = 32 - curPos;

		if (len - pos >= room)
		{
			memcpy(curBuf + 32 + curPos, data + pos, room);
			haraka512_port(result, curBuf);
			tmp = curBuf;
			curBuf = result;
			result = tmp;
			pos += room;
			curPos = 0;
		}
		else
		{
			memcpy(curBuf + 32 + curPos, data + pos, len - pos);
			curPos += len - pos;
			pos = len;
		}
	}

	memcpy(curBuf + 47, curBuf, 16);
	memcpy(curBuf + 63, curBuf, 1);
//	FillExtra((u128 *)curBuf);
	memcpy(result2, curBuf, 64);
};

extern "C" void Verus2hash(unsigned char *hash, unsigned char *curBuf, uint32_t nonce, uint32_t thr_id)
{
	uint64_t mask = VERUS_KEY_SIZE128; //552

//	GenNewCLKey(curBuf, data_key[thr_id]);  //data_key a global static 2D array data_key[16][8832];
	((uint32_t*)&curBuf[0])[8] = nonce;
	uint64_t intermediate = verusclhash_port(data_key[thr_id],curBuf, VERUS_KEY_SIZE);
		//FillExtra
	memcpy(curBuf + 47, &intermediate, 8);
	memcpy(curBuf + 55, &intermediate, 8);
	memcpy(curBuf + 63, &intermediate, 1);

	haraka512_port_keyed(hash, curBuf, data_key[thr_id] + (intermediate & mask));
}


extern "C" int scanhash_verus(int thr_id, struct work *work, uint32_t max_nonce, unsigned long *hashes_done)
{

	uint32_t _ALIGN(64) endiandata[35];
	uint32_t *pdata = work->data;
	uint32_t *ptarget = work->target;
	int dev_id = device_map[thr_id];
	uint8_t blockhash_half[64] = { 0 };
	struct timeval tv_start, tv_end, diff;
	double secs, solps;

	uint32_t nonce_buf = 0;
	uint32_t intensity = 20;

	unsigned char block_41970[] = { 0xfd, 0x40, 0x05, 0x01 };
	uint8_t _ALIGN(64) full_data[140 + 3 + 1344] = { 0 };
	uint8_t* sol_data = &full_data[140];
	
	memcpy(endiandata, pdata, 140);
	memcpy(sol_data, block_41970, 4);
	memcpy(full_data, endiandata, 140);

	throughput = cuda_default_throughput(thr_id, 1U << intensity);
	if (init[thr_id]) throughput = min(throughput, max_nonce - nonce_buf);
	
	if (!init[thr_id])
	{
		cudaSetDevice(dev_id);
		if (opt_cudaschedule == -1 && gpu_threads == 1) {
			cudaDeviceReset();
			// reduce cpu usage
			cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);
			CUDA_LOG_ERROR();
		}
		//cuda_get_arch(thr_id);
		gpulog(LOG_INFO, thr_id, "Intensity set to %g, %u cuda threads", throughput2intensity(throughput), throughput);
		verus_init(thr_id);
		init[thr_id] = true;
	}

	uint32_t _ALIGN(64) vhash[8] = {0};
    work->valid_nonces = 0;


	VerusHashHalf(blockhash_half,(unsigned char*) full_data, 1487);
	GenNewCLKey((unsigned char*)blockhash_half, data_key[thr_id]);  //data_key a global static 2D array data_key[16][8832];
	//Verus2hash((unsigned char *)vhash, (unsigned char *)blockhash_half, 0, thr_id);

	gettimeofday(&tv_start, NULL);
	verus_setBlock(blockhash_half, work->target, (uint8_t*)data_key[thr_id], thr_id); //set data to gpu kernel

	do {

		*hashes_done = nonce_buf + throughput;
		verus_hash(thr_id, throughput, nonce_buf, work->nonces);

		if (work->nonces[0] != UINT32_MAX)
		{
			const uint32_t Htarg = ptarget[7];
			
		//	Verus2hash((unsigned char *)vhash, (unsigned char *)blockhash_half, work->nonces[0], thr_id);


			if (vhash[7] <= Htarg && fulltest(vhash, ptarget))
			{
				*((uint32_t *)full_data + 368) = work->nonces[0];
				work->valid_nonces++;

				memcpy(work->data, endiandata, 140);
				int nonce = work->valid_nonces - 1;
				memcpy(work->extra, sol_data, 1347);
				bn_store_hash_target_ratio(vhash, work->target, work, nonce);

				work->nonces[work->valid_nonces - 1] = endiandata[NONCE_OFT];
				//pdata[NONCE_OFT] = endiandata[NONCE_OFT] + 1;
				goto out;
			}
			else if (vhash[7] > Htarg) {
				gpu_increment_reject(thr_id);
				if (!opt_quiet)
					gpulog(LOG_WARNING, thr_id, "nonce %08x does not validate on CPU!", work->nonces[0]);	
			}
		}
		if ((uint64_t)throughput + (uint64_t)nonce_buf >= (uint64_t)max_nonce) {

			break;
		}
		nonce_buf += throughput;

	} while (!work_restart[thr_id].restart);


out:
	gettimeofday(&tv_end, NULL);
	timeval_subtract(&diff, &tv_end, &tv_start);
	secs = (1.0 * diff.tv_sec) + (0.000001 * diff.tv_usec);
	solps = (double)nonce_buf / secs;
	//gpulog(LOG_INFO, thr_id, "%d k/hashes in %.2f s (%.2f MH/s)", nonce_buf / 1000, secs, solps / 1000000);
	// H/s

	//*hashes_done = first_nonce;
	pdata[NONCE_OFT] = endiandata[NONCE_OFT] + 1;
	//free(localkey);
	return work->valid_nonces;
}

// cleanup
void free_verushash(int thr_id)
{
	if (!init[thr_id])
		return;



	init[thr_id] = false;
}
