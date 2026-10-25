/*
LDPC SISO layered decoder

Copyright 2018 Ahmet Inan <xdsopl@gmail.com>
*/

#ifndef LAYERED_DECODER_HH
#define LAYERED_DECODER_HH

#include <stdlib.h>
#include "ldpc.hh"
#include <volk/volk_alloc.hh>

template <typename TYPE, typename ALG>
class LDPCDecoder
{
	TYPE *bnl, *pty;
	uint16_t *pos;
	uint8_t *cnc;
	ALG alg;
	int M, N, K, R, q, CNL, LT;
	bool initialized;
	// Per-lane early termination. active_mask holds 0xFF in the lanes still being
	// updated and 0x00 in converged lanes; the batch stops once every lane is 0x00.
	// Off by default: measured neutral-to-negative on both test captures. On a clean
	// signal the aggregate bad() check already exits after ~1 iteration, and on a hard
	// signal lanes rarely converge individually, so the extra syndrome sweep costs more
	// than it saves. Enable to protect converged lanes from straggler-induced updates
	// on marginal-SNR captures.
	bool early_termination = false;
	int min_iterations = 1;
	bool lane_active[TYPE::SIZE];
	int lanes_active = TYPE::SIZE;
	TYPE active_mask;

	void reset()
	{
		for (int i = 0; i < LT; ++i)
			bnl[i] = alg.zero();
	}
	bool bad(TYPE *data, TYPE *parity, int blocks)
	{
		for (int i = 0; i < q; ++i)
		{
			int cnt = cnc[i];
			for (int j = 0; j < M; ++j)
			{
				TYPE cnv = alg.sign(alg.one(), parity[M * i + j]);
				if (i)
					cnv = alg.sign(cnv, parity[M * (i - 1) + j]);
				else if (j)
					cnv = alg.sign(cnv, parity[j + (q - 1) * M - 1]);
				for (int c = 0; c < cnt; ++c)
					cnv = alg.sign(cnv, data[pos[CNL * (M * i + j) + c]]);
				if (alg.bad(cnv, blocks))
					return true;
			}
		}
		return false;
	}
	// Writes newv unless the lane is frozen, in which case the old value is kept.
	void store(TYPE *dst, TYPE newv, bool frozen)
	{
		if (!frozen)
		{
			*dst = newv;
			return;
		}
		TYPE oldv = *dst;
		*dst = vreinterpret<TYPE>(vbsl(vmask(active_mask), vmask(newv), vmask(oldv)));
	}
	void update(TYPE *data, TYPE *parity)
	{
		const bool frozen = lanes_active < TYPE::SIZE;
		// Scratch for the check-node update. Check-node degree is bounded by
		// links_max_cn() (30 for the largest DVB-S2 table), so a fixed buffer keeps
		// this off the heap - it used to allocate two vectors per check node.
		TYPE inp[64], out[64];
		TYPE *bl = bnl;
		for (int i = 0; i < q; ++i)
		{
			int cnt = cnc[i];
			for (int j = 0; j < M; ++j)
			{
				int deg = cnt + 2 - !(i | j);
				for (int c = 0; c < cnt; ++c)
					inp[c] = out[c] = alg.sub(data[pos[CNL * (M * i + j) + c]], bl[c]);
				inp[cnt] = out[cnt] = alg.sub(parity[M * i + j], bl[cnt]);
				if (i)
					inp[cnt + 1] = out[cnt + 1] = alg.sub(parity[M * (i - 1) + j], bl[cnt + 1]);
				else if (j)
					inp[cnt + 1] = out[cnt + 1] = alg.sub(parity[j + (q - 1) * M - 1], bl[cnt + 1]);
				alg.finalp(out, deg);
				for (int d = 0; d < deg; ++d)
				{
					if (frozen)
					{
						TYPE oldv = bl[d];
						alg.update(bl + d, out[d]);
						bl[d] = vreinterpret<TYPE>(vbsl(vmask(active_mask), vmask(bl[d]), vmask(oldv)));
					}
					else
						alg.update(bl + d, out[d]);
				}
				for (int c = 0; c < cnt; ++c)
					store(&data[pos[CNL * (M * i + j) + c]], alg.add(inp[c], bl[c]), frozen);
				store(&parity[M * i + j], alg.add(inp[cnt], bl[cnt]), frozen);
				if (i)
					store(&parity[M * (i - 1) + j], alg.add(inp[cnt + 1], bl[cnt + 1]), frozen);
				else if (j)
					store(&parity[j + (q - 1) * M - 1], alg.add(inp[cnt + 1], bl[cnt + 1]), frozen);
				bl += deg;
			}
		}
	}

	void serial_to_parallel(TYPE *data, code_type *code)
	{
		for (int n = 0; n < TYPE::SIZE; n++)
		{
			for (int j = 0; j < N; j++)
			{
				reinterpret_cast<code_type *>(data + j)[n] = code[(n * N) + j];
			}
		}
	}

	void parallel_to_serial(TYPE *data, code_type *code)
	{
		for (int n = 0; n < TYPE::SIZE; n++)
		{
			for (int j = 0; j < N; j++)
			{
				code[(n * N) + j] = reinterpret_cast<code_type *>(data + j)[n];
			}
		}
	}

public:
	LDPCDecoder() : initialized(false)
	{
	}
	// Per-lane early termination matches the strict all-or-nothing criterion in
	// behaviour (same frames decode) but lets converged lanes stop early.
	void set_early_termination(bool enabled)
	{
		early_termination = enabled;
	}
	void set_min_iterations(int n)
	{
		min_iterations = n < 1 ? 1 : n;
	}
	void init(LDPCInterface *it)
	{
		if (initialized)
		{
			volk_free(bnl);
			volk_free(pty);
			delete[] cnc;
			delete[] pos;
		}
		initialized = true;
		LDPCInterface *ldpc = it->clone();
		N = ldpc->code_len();
		K = ldpc->data_len();
		M = ldpc->group_len();
		R = N - K;
		q = R / M;
		CNL = ldpc->links_max_cn() - 2;
		pos = new uint16_t[R * CNL];
		cnc = new uint8_t[R];
		for (int i = 0; i < R; ++i)
			cnc[i] = 0;
		ldpc->first_bit();
		for (int j = 0; j < K; ++j)
		{
			int *acc_pos = ldpc->acc_pos();
			int bit_deg = ldpc->bit_deg();
			for (int n = 0; n < bit_deg; ++n)
			{
				int i = acc_pos[n];
				pos[CNL * i + cnc[i]++] = j;
			}
			ldpc->next_bit();
		}
		LT = ldpc->links_total();
		delete ldpc;
		bnl = reinterpret_cast<TYPE *>(volk_malloc(sizeof(TYPE) * LT, sizeof(TYPE)));
		pty = reinterpret_cast<TYPE *>(volk_malloc(sizeof(TYPE) * R, sizeof(TYPE)));
		uint16_t *tmp = new uint16_t[R * CNL];
		for (int i = 0; i < q; ++i)
			for (int j = 0; j < M; ++j)
				for (int c = 0; c < CNL; ++c)
					tmp[CNL * (M * i + j) + c] = pos[CNL * (q * j + i) + c];
		delete[] pos;
		pos = tmp;
	}
	// Recompute which lanes still have an unsatisfied check, and refresh active_mask.
	// Lanes that were already frozen stay frozen; lanes that have just converged join them.
	// Returns the number of lanes still active.
	int refresh_mask(TYPE *data, TYPE *parity)
	{
		if (lanes_active == 0)
			return 0;
		bool bad_lane[TYPE::SIZE];
		for (int i = 0; i < TYPE::SIZE; ++i)
			bad_lane[i] = false;
		for (int i = 0; i < q; ++i)
		{
			int cnt = cnc[i];
			for (int j = 0; j < M; ++j)
			{
				TYPE cnv = alg.sign(alg.one(), parity[M * i + j]);
				if (i)
					cnv = alg.sign(cnv, parity[M * (i - 1) + j]);
				else if (j)
					cnv = alg.sign(cnv, parity[j + (q - 1) * M - 1]);
				for (int c = 0; c < cnt; ++c)
					cnv = alg.sign(cnv, data[pos[CNL * (M * i + j) + c]]);
				alg.bad_lanes(cnv, bad_lane);
			}
		}
		lanes_active = 0;
		for (int i = 0; i < TYPE::SIZE; ++i)
		{
			if (lane_active[i] && !bad_lane[i])
				lane_active[i] = false;
			if (lane_active[i])
				++lanes_active;
			active_mask.u[i] = lane_active[i] ? 0xFF : 0x00;
		}
		return lanes_active;
	}
	void reset_mask()
	{
		for (int i = 0; i < TYPE::SIZE; ++i)
			lane_active[i] = true;
		lanes_active = TYPE::SIZE;
		active_mask = vdup<TYPE>(-1);
	}
	int operator()(void *buffer, code_type *code, int trials = 25)
	{
		const int blocks = TYPE::SIZE;
		TYPE *data = reinterpret_cast<TYPE *>(buffer);
		TYPE *parity = data + K;
		serial_to_parallel(data, code);
		reset();
		for (int i = 0; i < q; ++i)
			for (int j = 0; j < M; ++j)
				pty[M * i + j] = parity[q * j + i];
		if (!early_termination)
		{
			// Original behaviour: all-or-nothing, bit-identical to the unmodified decoder.
			while (bad(data, pty, blocks) && --trials >= 0)
				update(data, pty);
		}
		else
		{
			// Per-lane: converged lanes stop being updated and the batch exits as soon as
			// the last lane converges. Iteration accounting matches the loop above exactly.
			int it = 0;
			reset_mask();
			if (refresh_mask(data, pty) != 0)
			{
				while (--trials >= 0)
				{
					update(data, pty);
					++it;
					if (it >= min_iterations && refresh_mask(data, pty) == 0)
						break;
				}
			}
		}
		for (int i = 0; i < q; ++i)
			for (int j = 0; j < M; ++j)
				parity[q * j + i] = pty[M * i + j];
		parallel_to_serial(data, code);
		return trials;
	}
	~LDPCDecoder()
	{
		if (initialized)
		{
			volk_free(bnl);
			volk_free(pty);
			delete[] cnc;
			delete[] pos;
		}
	}
};

#endif
