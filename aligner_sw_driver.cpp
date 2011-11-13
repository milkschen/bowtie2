/*
 * Copyright 2011, Ben Langmead <blangmea@jhsph.edu>
 *
 * This file is part of Bowtie 2.
 *
 * Bowtie 2 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Bowtie 2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Bowtie 2.  If not, see <http://www.gnu.org/licenses/>.
 */

#define TIMER_START() \
	struct timeval tv_i, tv_f; \
	struct timezone tz_i, tz_f; \
	size_t total_usecs; \
	gettimeofday(&tv_i, &tz_i)

#define IF_TIMER_END() \
	gettimeofday(&tv_f, &tz_f); \
	total_usecs = \
		(tv_f.tv_sec - tv_i.tv_sec) * 1000000 + (tv_f.tv_usec - tv_i.tv_usec); \
	if(total_usecs > 300000)

/*
 * aligner_sw_driver.cpp
 *
 * Routines that drive the alignment process given a collection of seed hits.
 * This is generally done in a few stages: extendSeeds visits the set of
 * seed-hit BW elements in some order; for each element visited it resolves its
 * reference offset; once the reference offset is known, bounds for a dynamic
 * programming subproblem are established; if these bounds are distinct from
 * the bounds we've already tried, we solve the dynamic programming subproblem
 * and report the hit; if the AlnSinkWrap indicates that we can stop, we
 * return, otherwise we continue on to the next BW element.
 */

#include <iostream>
#include <algorithm>
#include "aligner_cache.h"
#include "aligner_sw_driver.h"
#include "pe.h"
#include "dp_framer.h"
// -- BTL remove --
#include <stdlib.h>
#include <sys/time.h>
// -- --

using namespace std;

void SwDriver::resolveAll(
	SeedResults& sh,             // seed hits to extend into full alignments
	const Ebwt& ebwt,            // BWT
	const BitPairReference& ref, // Reference strings
	AlignmentCacheIface& ca,     // alignment cache for seed hits
	RandomSource& rnd,           // pseudo-random generator
	WalkMetrics& wlm,            // group walk left metrics
	size_t& nelt_out)            // out: # elements total
{
	const size_t nonz = sh.nonzeroOffsets(); // non-zero positions
	gws_.clear();
	satpos_.clear();
	for(size_t i = 0; i < nonz; i++) {
		satups_.clear();
		bool fw = true;
		uint32_t offidx = 0, rdoff = 0, seedlen = 0;
		QVal qv = sh.hitsByRank(i, offidx, rdoff, fw, seedlen);
		assert(qv.valid());
		assert(!qv.empty());
		assert(qv.repOk(ca.current()));
		size_t nrange = 0;
		ca.queryQval(qv, satups_, nrange, nelt_out);
		for(size_t j = 0; j < satups_.size(); j++) {
			satpos_.expand();
			satpos_.back().sat = satups_[j];
			satpos_.back().origSz = satups_[j].size();
			satpos_.back().pos.init(fw, offidx, rdoff, seedlen);
			gws_.expand();
			gws_.back().init(
				ebwt,               // forward Bowtie index
				ref,                // reference sequences
				satpos_.back().sat, // SA tuples: ref hit, salist range
				NULL,               // Combiner for resolvers
				rnd,                // pseudo-random generator
				wlm);               // metrics
			TIMER_START();
			gws_.back().resolveAll(wlm);
			IF_TIMER_END() {
				cerr << "Saw a long resolveAll (" << total_usecs << ")" << endl;
			}
		}
	}
}

/**
 * Given end-to-end alignment results stored in the SeedResults structure, set
 * up all of our state for resolving and keeping track of reference offsets for
 * hits.  Order the list of ranges to examine such that all exact end-to-end
 * alignments are examined before any 1mm end-to-end alignments.
 */
bool SwDriver::eeSaTups(
	SeedResults& sh,             // seed hits to extend into full alignments
	const Ebwt& ebwt,            // BWT
	const BitPairReference& ref, // Reference strings
	RandomSource& rnd,           // pseudo-random generator
	WalkMetrics& wlm,            // group walk left metrics
	SwMetrics& swmSeed,          // metrics for seed extensions
	size_t& nelt_out)            // out: # elements total
{
	gws_.clear();
	rands_.clear();
	satpos_.clear();
	eehits_.clear();
	// First, count up the total number of satpos_, rands_, eehits_, and gws_
	// we're going to tuse
	size_t nobj = 0;
	if(!sh.exactFwEEHit().empty()) nobj++;
	if(!sh.exactRcEEHit().empty()) nobj++;
	nobj += sh.mm1EEHits().size();
	gws_.ensure(nobj);
	rands_.ensure(nobj);
	satpos_.ensure(nobj);
	eehits_.ensure(nobj);	
	size_t tot = sh.exactFwEEHit().size() + sh.exactRcEEHit().size();
	bool succ = false;
	bool firstEe = true;
	if(tot > 0) {
		uint32_t rn = rnd.nextU32() % (uint32_t)tot;
		for(int fwi = 0; fwi < 2; fwi++) {
			bool fw = (fwi == 0);
			if(rn >= sh.exactFwEEHit().size()) {
				fw = !fw;
			}
			EEHit hit = fw ? sh.exactFwEEHit() : sh.exactRcEEHit();
			if(hit.empty()) {
				continue;
			}
			assert(hit.fw == fw);
			if(hit.bot > hit.top) {
				// Clear list where resolved offsets are stored
				swmSeed.exranges++;
				swmSeed.exrows += (hit.bot - hit.top);
				if(!succ) {
					swmSeed.exsucc++;
					succ = true;
				}
				if(firstEe) {
					salistEe_.clear();
					pool_.clear();
					firstEe = false;
				}
				TSlice o(salistEe_, (uint32_t)salistEe_.size(), hit.bot - hit.top);
				for(size_t i = 0; i < hit.bot - hit.top; i++) {
					if(!salistEe_.add(pool_, 0xffffffff)) {
						swmSeed.exooms++;
						return false;
					}
				}
				eehits_.push_back(hit);
				satpos_.expand();
				satpos_.back().sat.init(SAKey(), hit.top, o);
				satpos_.back().sat.key.seq = std::numeric_limits<uint64_t>::max();
				satpos_.back().sat.key.len = (uint32_t)sh.readLength();
				satpos_.back().pos.init(hit.fw, 0, 0, (uint32_t)sh.readLength());
				satpos_.back().origSz = hit.bot - hit.top;
				rands_.expand();
				rands_.back().init(hit.bot - hit.top);
				gws_.expand();
				gws_.back().init(
					ebwt,               // forward Bowtie index
					ref,                // reference sequences
					satpos_.back().sat, // SATuple
					NULL,               // Combiner for resolvers
					rnd,                // pseudo-random generator
					wlm);               // metrics
				assert(gws_.back().repOk());
				nelt_out += (hit.bot - hit.top);
			}
		}
	}
	succ = false;
	if(!sh.mm1EEHits().empty()) {
		sh.sort1mmEe();
		size_t sz = sh.mm1EEHits().size();
		for(size_t i = 0; i < sz; i++) {
			EEHit hit = sh.mm1EEHits()[i];
			assert(!hit.empty());
			// Clear list where resolved offsets are stored
			swmSeed.mm1ranges++;
			swmSeed.mm1rows += (hit.bot - hit.top);
			if(!succ) {
				swmSeed.mm1succ++;
				succ = true;
			}
			if(firstEe) {
				salistEe_.clear();
				pool_.clear();
				firstEe = false;
			}
			TSlice o(salistEe_, (uint32_t)salistEe_.size(), hit.bot - hit.top);
			for(size_t i = 0; i < hit.bot - hit.top; i++) {
				if(!salistEe_.add(pool_, 0xffffffff)) {
					swmSeed.mm1ooms++;
					return false;
				}
			}
			eehits_.push_back(hit);
			satpos_.expand();
			satpos_.back().sat.init(SAKey(), hit.top, o);
			satpos_.back().sat.key.seq = std::numeric_limits<uint64_t>::max();
			satpos_.back().sat.key.len = (uint32_t)sh.readLength();
			satpos_.back().pos.init(hit.fw, 0, 0, (uint32_t)sh.readLength());
			satpos_.back().origSz = hit.bot - hit.top;
			rands_.expand();
			rands_.back().init(hit.bot - hit.top);
			gws_.expand();
			gws_.back().init(
				ebwt,               // forward Bowtie index
				ref,                // reference sequences
				satpos_.back().sat, // SATuple
				NULL,               // Combiner for resolvers
				rnd,                // pseudo-random generator
				wlm);               // metrics
			assert(gws_.back().repOk());
			nelt_out += (hit.bot - hit.top);
		}
	}
	return true;
}

/**
 * Given seed results, set up all of our state for resolving and keeping
 * track of reference offsets for hits.
 */
void SwDriver::prioritizeSATups(
	SeedResults& sh,             // seed hits to extend into full alignments
	const Ebwt& ebwt,            // BWT
	const BitPairReference& ref, // Reference strings
	bool refscan,                // Use reference scanning
	size_t maxelt,               // max elts we'll consider
	size_t nsm,                  // if range as <= nsm elts, it's "small"
	AlignmentCacheIface& ca,     // alignment cache for seed hits
	RandomSource& rnd,           // pseudo-random generator
	WalkMetrics& wlm,            // group walk left metrics
	size_t& nelt_out)            // out: # elements total
{
	const size_t nonz = sh.nonzeroOffsets(); // non-zero positions
	satups_.clear();
	gws_.clear();
	rands_.clear();
	rands2_.clear();
	satpos_.clear();
	satpos2_.clear();
	if(refscan) {
		sacomb_.clear();
		sstab_.init(4); // Seed = 4 DNA chars = 1 byte
	}
	size_t nrange = 0, nelt = 0, nsmall = 0, nsmall_elts = 0;
	bool keepWhole = false;
	for(size_t i = 0; i < nonz; i++) {
		bool fw = true;
		uint32_t offidx = 0, rdoff = 0, seedlen = 0;
		QVal qv = sh.hitsByRank(i, offidx, rdoff, fw, seedlen);
		assert(qv.valid());
		assert(!qv.empty());
		assert(qv.repOk(ca.current()));
		ca.queryQval(qv, satups_, nrange, nelt);
		for(size_t j = 0; j < satups_.size(); j++) {
			const size_t sz = satups_[j].size();
			if(sz <= nsm) {
				nsmall++;
				nsmall_elts += sz;
			}
			if(keepWhole) {
				satpos_.expand();
				satpos_.back().sat = satups_[j];
				satpos_.back().origSz = sz;
				satpos_.back().pos.init(fw, offidx, rdoff, seedlen);
			} else {
				satpos2_.expand();
				satpos2_.back().sat = satups_[j];
				satpos2_.back().origSz = sz;
				satpos2_.back().pos.init(fw, offidx, rdoff, seedlen);
			}
		}
		satups_.clear();
	}
	assert_leq(nsmall, nrange);
	nelt_out = nelt; // return the total number of elements
	if(keepWhole) {
		assert_eq(nrange, satpos_.size());
		satpos_.sort();
	} else {
		assert_eq(nrange, satpos2_.size());
		satpos2_.sort();
	}
	if(keepWhole) {
		if(refscan) {
			sacomb_.ensure(nrange);
		}
		gws_.ensure(nrange);
		rands_.ensure(nrange);
		for(size_t i = 0; i < nrange; i++) {
			if(refscan) {
				sacomb_.expand();
				sacomb_.back().init(satpos_[i].sat);
			}
			gws_.expand();
			gws_.back().init(
				ebwt,           // forward Bowtie index
				ref,            // reference sequences
				satpos_[i].sat, // SA tuples: ref hit, salist range
				refscan ? &sacomb_.back() : NULL, // Combiner for resolvers
				rnd,            // pseudo-random generator
				wlm);           // metrics
			assert(gws_.back().initialized());
			rands_.expand();
			rands_.back().init(satpos_[i].sat.size());
		}
		return;
	}
	// Resize satups_ list so that ranges having elements that we might
	// possibly explore are present
	satpos_.ensure(min(maxelt, nelt));
	if(refscan) {
		sacomb_.ensure(min(maxelt, nelt));
	}
	gws_.ensure(min(maxelt, nelt));
	rands_.ensure(min(maxelt, nelt));
	rands2_.ensure(min(maxelt, nelt));
	size_t nlarge_elts = nelt - nsmall_elts;
	if(maxelt < nelt) {
		size_t diff = nelt - maxelt;
		if(diff >= nlarge_elts) {
			nlarge_elts = 0;
		} else {
			nlarge_elts -= diff;
		}
	}
	size_t nelt_added = 0;
	// Now we have a collection of ranges in satpos2_.  Now we want to decide
	// how we explore elements from them.  The basic idea is that: for very
	// small guys, where "very small" means that the size of the range is less
	// than or equal to the parameter 'nsz', we explore them in their entirety
	// right away.  For the rest, we want to select in a way that is (a)
	// random, and (b) weighted toward examining elements from the smaller
	// ranges more frequently (and first).
	//
	// 1. do the smalls
	for(size_t j = 0; j < nsmall && nelt_added < maxelt; j++) {
		satpos_.expand();
		satpos_.back() = satpos2_[j];
		// The following mechanism for ensuring we don't go over the maxelt
		// limit is tricky because it can mess us up when we try to combine
		// ref-scanning results with BW search results.
		
		//if(nelt_added + satpos_.back().sat.size() > maxelt) {
		//	// Curtail so as not to exceed maxelt
		//	size_t nlen = maxelt - nelt_added;
		//	satpos_.back().sat.setLength(nlen);
		//}
		if(refscan) {
			sstab_.add(
				make_pair(j, 0),
				satpos2_[j].sat.key.seq,
				(size_t)satpos2_[j].pos.seedlen);
			sacomb_.expand();
			sacomb_.back().init(satpos_.back().sat);
		}
		gws_.expand();
		gws_.back().init(
			ebwt,               // forward Bowtie index
			ref,                // reference sequences
			satpos_.back().sat, // SA tuples: ref hit, salist range
			refscan ? &sacomb_.back() : NULL, // Combiner for resolvers
			rnd,                // pseudo-random generator
			wlm);               // metrics
		assert(gws_.back().initialized());
		rands_.expand();
		rands_.back().init(satpos_.back().sat.size());
		nelt_added += satpos_.back().sat.size();
#ifndef NDEBUG
		for(size_t k = 0; k < satpos_.size()-1; k++) {
			assert(!(satpos_[k] == satpos_.back()));
		}
#endif
	}
	if(nelt_added >= maxelt || nsmall == satpos2_.size()) {
		return;
	}
	// 2. do the non-smalls
	// Initialize the row sampler
	rowsamp_.init(satpos2_, nsmall, satpos2_.size());
	// Initialize the random choosers
	rands2_.resize(satpos2_.size());
	for(size_t j = 0; j < satpos2_.size(); j++) {
		rands2_[j].reset();
	}
	while(nelt_added < maxelt && nelt_added < nelt) {
		size_t ri = rowsamp_.next(rnd) + nsmall;
		assert_geq(ri, nsmall);
		assert_lt(ri, satpos2_.size());
		if(!rands2_[ri].inited()) {
			rands2_[ri].init(satpos2_[ri].sat.size());
			assert(!rands2_[ri].done());
		}
		assert(!rands2_[ri].done());
		uint32_t r = rands2_[ri].next(rnd);
		if(rands2_[ri].done()) {
			// Tell the row sampler this range is done
			rowsamp_.finishedRange(ri - nsmall);
		}
		SATuple sa;
		TSlice o;
		o.init(satpos2_[ri].sat.offs, r, r+1);
		sa.init(satpos2_[ri].sat.key, satpos2_[ri].sat.top + r, o);
		satpos_.expand();
		satpos_.back().sat = sa;
		satpos_.back().origSz = satpos2_[ri].origSz;
		satpos_.back().pos = satpos2_[ri].pos;
		if(refscan) {
			sacomb_.expand();
			sacomb_.back().reset();
		}
		gws_.expand();
		gws_.back().init(
			ebwt,               // forward Bowtie index
			ref,                // reference sequences
			satpos_.back().sat, // SA tuples: ref hit, salist range
			refscan ? &sacomb_.back() : NULL, // Combiner for resolvers
			rnd,                // pseudo-random generator
			wlm);               // metrics
		assert(gws_.back().initialized());
		rands_.expand();
		rands_.back().init(1);
		nelt_added++;
	}
}

enum {
	FOUND_NONE = 0,
	FOUND_EE,
	FOUND_UNGAPPED,
};

/**
 * Given a collection of SeedHits for a single read, extend seed alignments
 * into full alignments.  Where possible, try to avoid redundant offset lookups
 * and dynamic programming wherever possible.  Optionally report alignments to
 * a AlnSinkWrap object as they are discovered.
 *
 * If 'reportImmediately' is true, returns true iff a call to msink->report()
 * returned true (indicating that the reporting policy is satisfied and we can
 * stop).  Otherwise, returns false.
 */
bool SwDriver::extendSeeds(
	const Read& rd,              // read to align
	bool mate1,                  // true iff rd is mate #1
	bool color,                  // true -> read is colorspace
	SeedResults& sh,             // seed hits to extend into full alignments
	const Ebwt& ebwt,            // BWT
	const BitPairReference& ref, // Reference strings
	SwAligner& swa,              // dynamic programming aligner
	const Scoring& sc,           // scoring scheme
	int seedmms,                 // # mismatches allowed in seed
	int seedlen,                 // length of seed
	int seedival,                // interval between seeds
	TAlScore minsc,              // minimum score for anchor
	TAlScore floorsc,            // local-alignment floor for anchor score
	int nceil,                   // maximum # Ns permitted in reference portion
	const SimpleFunc& maxeltf,   // # elts to explore as function of total elts
	size_t maxhalf,  	         // max width in either direction for DP tables
	bool doUngapped,             // do ungapped alignment
	size_t ungappedThresh,       // all attempts after this many are ungapped
	bool enable8,                // use 8-bit SSE where possible
	bool refscan,                // use reference scanning
	int tighten,                 // -M score tightening mode
	AlignmentCacheIface& ca,     // alignment cache for seed hits
	RandomSource& rnd,           // pseudo-random source
	WalkMetrics& wlm,            // group walk left metrics
	SwMetrics& swmSeed,          // DP metrics for seed-extend
	AlnSinkWrap* msink,          // AlnSink wrapper for multiseed-style aligner
	bool reportImmediately,      // whether to report hits immediately to msink
	bool& exhaustive)            // set to true iff we searched all seeds exhaustively
{
	//TIMER_START();
	typedef std::pair<uint32_t, uint32_t> U32Pair;

	assert(!reportImmediately || msink != NULL);
	assert(!reportImmediately || msink->empty());
	assert(!reportImmediately || !msink->maxed());
	assert_gt(seedlen, 0);

	assert_geq(nceil, 0);
	assert_leq((size_t)nceil, rd.length());
	
	// Calculate the largest possible number of read and reference gaps
	const size_t rdlen = rd.length();
	TAlScore perfectScore = sc.perfectScore(rdlen);

	DynProgFramer dpframe(!gReportOverhangs);
	swa.reset();

	// Initialize a set of GroupWalks, one for each seed.  Also, intialize the
	// accompanying lists of reference seed hits (satups*) and the combiners
	// that link the reference-scanning results to the BW walking results
	// (sacomb_).  Finally, initialize the SeedScanTable and SeedScanner
	// objects that will actually do the reference scanning and opportunistic
	// resolution of offsets.
	const size_t nsm = 3;
	const size_t nonz = sh.nonzeroOffsets(); // non-zero positions
	assert_gt(nonz, 0);
	double maxelt_db = maxeltf.f<double>((double)nonz);
	if(maxelt_db < std::numeric_limits<double>::max()) {
		maxelt_db *= 32.0/pow((double)max<size_t>(rdlen, 100), 0.75);
		maxelt_db = max<double>(maxelt_db, 2.0f);
	}
	size_t maxelt = (size_t)maxelt_db;
	if(maxelt_db == std::numeric_limits<double>::max()) {
		maxelt = std::numeric_limits<size_t>::max();
	}

	size_t eeHits = sh.numE2eHits();
	bool eeMode = eeHits > 0;
	bool firstEe = true;
	bool firstExtend = true;

	size_t nelt = 0, neltLeft = 0;
	size_t rows = rdlen + (color ? 1 : 0);
	size_t eltsDone = 0;
	while(true) {
		if(eeMode) {
			if(firstEe) {
				firstEe = false;
				eeMode = eeSaTups(
					sh,           // seed hits to extend into full alignments
					ebwt,         // BWT
					ref,          // Reference strings
					rnd,          // pseudo-random generator
					wlm,          // group walk left metrics
					swmSeed,      // seed-extend metrics
					nelt);        // out: # elements total
				assert_eq(gws_.size(), rands_.size());
				assert_eq(gws_.size(), satpos_.size());
			} else {
				eeMode = false;
			}
		}
		if(!eeMode) {
			if(minsc == perfectScore) {
				// Already found all perfect hits!
				return false;
			}
			if(firstExtend) {
				nelt = 0;
				prioritizeSATups(
					sh,           // seed hits to extend into full alignments
					ebwt,         // BWT
					ref,          // Reference strings
					refscan,      // do reference scanning?
					maxelt,       // max rows to consider per position
					nsm,          // smallness threshold
					ca,           // alignment cache for seed hits
					rnd,          // pseudo-random generator
					wlm,          // group walk left metrics
					nelt);        // out: # elements total
				assert_eq(gws_.size(), rands_.size());
				assert_eq(gws_.size(), satpos_.size());
				neltLeft = nelt;
				firstExtend = false;
			}
			if(!(neltLeft > 0 && eltsDone < maxelt)) {
				// Finished examining gapped candidates
				break;
			}
		}
		for(size_t i = 0; i < gws_.size(); i++) {
			if(eeMode && eehits_[i].score < minsc) {
				break;
			}
			bool small       = satpos_[i].sat.size() < nsm;
			bool fw          = satpos_[i].pos.fw;
			uint32_t rdoff   = satpos_[i].pos.rdoff;
			uint32_t seedlen = satpos_[i].pos.seedlen;
			if(!fw) {
				// 'rdoff' and 'offidx' are with respect to the 5' end of
				// the read.  Here we convert rdoff to be with respect to
				// the upstream (3') end of ther read.
				rdoff = (uint32_t)(rdlen - rdoff - seedlen);
			}
			bool first = true;
			assert_leq(eltsDone, maxelt);
			// If the range is small, investigate all elements now.  If the
			// range is large, just investigate one and move on - we might come
			// back to this range later.
			while(!rands_[i].done() &&
			      (eeMode ||
			      (eltsDone < maxelt && (first || small))))
			{
				if(minsc == perfectScore) {
					if(!eeMode || eehits_[i].score < perfectScore) {
						return false;
					}
				} else if(eeMode && eehits_[i].score < minsc) {
					break;
				}
				first = false;
				assert(!gws_[i].done());
				// Resolve next element offset
				WalkResult wr;
				uint32_t elt = rands_[i].next(rnd);
				gws_[i].advanceElement(elt, wr, wlm);
				if(!eeMode) {
					eltsDone++;
					assert_gt(neltLeft, 0);
					neltLeft--;
				}
				assert_neq(0xffffffff, wr.toff);
				uint32_t tidx = 0, toff = 0, tlen = 0;
				ebwt.joinedToTextOff(
					wr.elt.len,
					wr.toff,
					tidx,
					toff,
					tlen);
				tlen += (color ? 1 : 0);
				if(tidx == 0xffffffff) {
					// The seed hit straddled a reference boundary so the seed hit
					// isn't valid
					continue;
				}
#ifndef NDEBUG
				if(!eeMode) { // Check that seed hit matches reference
					uint64_t key = satpos_[i].sat.key.seq;
					for(size_t k = 0; k < wr.elt.len; k++) {
						int c = ref.getBase(tidx, toff + wr.elt.len - k - 1);
						int ck = (int)(key & 3);
						key >>= 2;
						assert_eq(c, ck);
					}
				}
#endif
				// Find offset of alignment's upstream base assuming net gaps=0
				// between beginning of read and beginning of seed hit
				int64_t refoff = (int64_t)toff - rdoff;
				// Coordinate of the seed hit w/r/t the pasted reference string
				Coord refcoord(tidx, refoff, fw);
				if(seenDiags1_.locusPresent(refcoord)) {
					// Already handled alignments seeded on this diagonal
					swmSeed.rshit++;
					continue;
				}
				// Now that we have a seed hit, there are many issues to solve
				// before we have a completely framed dynamic programming problem.
				// They include:
				//
				// 1. Setting reference offsets on either side of the seed hit,
				//    accounting for where the seed occurs in the read
				// 2. Adjusting the width of the banded dynamic programming problem
				//    and adjusting reference bounds to allow for gaps in the
				//    alignment
				// 3. Accounting for the edges of the reference, which can impact
				//    the width of the DP problem and reference bounds.
				// 4. Perhaps filtering the problem down to a smaller problem based
				//    on what DPs we've already solved for this read
				//
				// We do #1 here, since it is simple and we have all the seed-hit
				// information here.  #2 and #3 are handled in the DynProgFramer.
				int readGaps = 0, refGaps = 0;
				bool ungapped = false;
				if(!eeMode) {
					readGaps = sc.maxReadGaps(minsc, rdlen);
					refGaps  = sc.maxRefGaps(minsc, rdlen);
					ungapped = (readGaps == 0 && refGaps == 0) || eltsDone >= ungappedThresh;
				}
				int state = FOUND_NONE;
				bool found = false;
				if(eeMode) {
					// Set up resEe_
					resEe_.reset();
					resEe_.alres.reset();
					const EEHit& h = eehits_[i];
					assert_leq(h.score, perfectScore);
					resEe_.alres.setScore(AlnScore(h.score, h.ns(), 0));
					resEe_.alres.setShape(
						refcoord.ref(),  // ref id
						refcoord.off(),  // 0-based ref offset
						fw,              // aligned to Watson?
						rdlen,           // read length
						false,           // read was colorspace?
						true,            // pretrim soft?
						0,               // pretrim 5' end
						0,               // pretrim 3' end
						true,            // alignment trim soft?
						0,               // alignment trim 5' end
						0);              // alignment trim 3' end
					resEe_.alres.setRefNs(h.refns());
					if(h.mms() > 0) {
						assert_eq(1, h.mms());
						resEe_.alres.ned().push_back(h.e1);
					}
					state = FOUND_EE;
					found = true;
					Interval refival(refcoord, 1);
					seenDiags1_.add(refival);
				} else if(doUngapped && ungapped) {
					resUngap_.reset();
					int al = swa.ungappedAlign(
						fw ? rd.patFw : rd.patRc,
						fw ? rd.qual  : rd.qualRev,
						refcoord,
						ref,
						tlen,
						sc,
						gReportOverhangs,
						minsc,
						floorsc,
						resUngap_);
					Interval refival(refcoord, 1);
					seenDiags1_.add(refival);
					if(al == 0) {
						swmSeed.ungapfail++;
						continue;
					} else if(al == -1) {
						swmSeed.ungapnodec++;
					} else {
						found = true;
						state = FOUND_UNGAPPED;
						swmSeed.ungapsucc++;
					}
				}
				int64_t pastedRefoff = (int64_t)wr.toff - rdoff;
				DPRect rect;
				if(state == FOUND_NONE) {
					found = dpframe.frameSeedExtensionRect(
						refoff,   // ref offset implied by seed hit assuming no gaps
						rows,     // length of read sequence used in DP table (so len
								  // of +1 nucleotide sequence for colorspace reads)
						tlen,     // length of reference
						readGaps, // max # of read gaps permitted in opp mate alignment
						refGaps,  // max # of ref gaps permitted in opp mate alignment
						(size_t)nceil, // # Ns permitted
						maxhalf,  // max width in either direction
						rect);    // DP rectangle
					assert(rect.repOk());
					// Add the seed diagonal at least
					seenDiags1_.add(Interval(refcoord, 1));
					if(!found) {
						continue;
					}
				}
				int64_t leftShift = refoff - rect.refl;
				size_t nwindow = 0;
				if(toff >= rect.refl) {
					nwindow = (size_t)(toff - rect.refl);
				}
				// NOTE: We might be taking off more than we should because the
				// pasted string omits non-A/C/G/T characters, but we included them
				// when calculating leftShift.  We'll account for this later.
				pastedRefoff -= leftShift;
				size_t nsInLeftShift = 0;
				if(state == FOUND_NONE) {
					sscan_.init(sstab_);
					if(!swa.initedRead()) {
						// Initialize the aligner with a new read
						swa.initRead(
							rd.patFw,  // fw version of query
							rd.patRc,  // rc version of query
							rd.qual,   // fw version of qualities
							rd.qualRev,// rc version of qualities
							0,         // off of first char in 'rd' to consider
							rdlen,     // off of last char (excl) in 'rd' to consider
							color,     // colorspace?
							sc,        // scoring scheme
							floorsc);  // local-alignment floor score
					}
					swa.initRef(
						fw,        // whether to align forward or revcomp read
						tidx,      // reference aligned against
						rect,      // DP rectangle
						ref,       // Reference strings
						tlen,      // length of reference sequence
						sc,        // scoring scheme
						minsc,     // minimum score permitted
						enable8,   // use 8-bit SSE if possible?
						true,      // this is a seed extension - not finding a mate
						&sscan_,   // reference scanner for resolving offsets
						nwindow,
						nsInLeftShift);
					if(refscan) {
						// Take reference-scanner hits and turn them into offset
						// resolutions.
						wlm.refscanhits += sscan_.hits().size();
						pastedRefoff += nsInLeftShift;
						for(size_t j = 0; j < sscan_.hits().size(); j++) {
							// Get identifier for the appropriate combiner
							U32Pair id = sscan_.hits()[j].id();
							// Get the hit's offset in pasted-reference coordinates
							int64_t off = sscan_.hits()[j].off() + pastedRefoff;
							assert_geq(off, sscan_.hits()[j].ns());
							off -= sscan_.hits()[j].ns();
							assert_geq(off, 0);
							assert_lt(off, (int64_t)ebwt.eh().lenNucs());
							assert_lt(off, (int64_t)0xffffffff);
							// Check that reference sequence actually matches seed
#ifndef NDEBUG
							uint32_t tidx2 = 0, toff2 = 0, tlen2 = 0;
							ebwt.joinedToTextOff(
								wr.elt.len,
								(uint32_t)off,
								tidx2,
								toff2,
								tlen2);
							assert_neq(0xffffffff, tidx2);
							//uint64_t key = sacomb_[id.first][id.second].satup().key.seq;
							uint64_t key = sacomb_[id.first].satup().key.seq;
							for(size_t k = 0; k < wr.elt.len; k++) {
								int c = ref.getBase(tidx2, toff2 + wr.elt.len - k - 1);
								int ck = (int)(key & 3);
								key >>= 2;
								assert_eq(c, ck);
							}
#endif
							// Install it
							if(sacomb_[id.first].addRefscan((uint32_t)off)) {
								// It was new; see if it leads to any resolutions
								sacomb_[id.first].tryResolving(wlm.refresolves);
							}
						}
					}
					// Because of how we framed the problem, we can say that we've
					// exhaustively scored the seed diagonal as well as maxgaps
					// diagonals on either side
					Interval refival(tidx, 0, fw, 0);
					rect.initIval(refival);
					seenDiags1_.add(refival);
					// Now fill the dynamic programming matrix and return true iff
					// there is at least one valid alignment
					found = swa.align(rnd);
					if(!found) {
						continue; // Look for more anchor alignments
					}
				}
				bool firstInner = true;
				while(true) {
					assert(found);
					SwResult *res = NULL;
					if(state == FOUND_EE) {
						if(!firstInner) {
							break;
						}
						res = &resEe_;
					} else if(state == FOUND_UNGAPPED) {
						if(!firstInner) {
							break;
						}
						res = &resUngap_;
					} else {
						resGap_.reset();
						assert(resGap_.empty());
						if(swa.done()) {
							break;
						}
						swa.nextAlignment(resGap_, minsc, rnd);
						found = !resGap_.empty();
						if(!found) {
							break;
						}
						res = &resGap_;
					}
					assert(res != NULL);
					firstInner = false;
					assert(res->alres.matchesRef(
						rd,
						ref,
						tmp_rf_,
						tmp_rdseq_,
						tmp_qseq_,
						raw_refbuf_,
						raw_destU32_,
						raw_matches_));
					Interval refival(tidx, 0, fw, tlen);
					assert_gt(res->alres.refExtent(), 0);
					if(gReportOverhangs &&
					   !refival.containsIgnoreOrient(res->alres.refival()))
					{
						res->alres.clipOutside(true, 0, tlen);
						if(res->alres.refExtent() == 0) {
							continue;
						}
					}
					assert(gReportOverhangs ||
					       refival.containsIgnoreOrient(res->alres.refival()));
					// Did the alignment fall entirely outside the reference?
					if(!refival.overlapsIgnoreOrient(res->alres.refival())) {
						continue;
					}
					// Is this alignment redundant with one we've seen previously?
					if(redAnchor_.overlap(res->alres)) {
						// Redundant with an alignment we found already
						continue;
					}
					redAnchor_.add(res->alres);
					// Annotate the AlnRes object with some key parameters
					// that were used to obtain the alignment.
					res->alres.setParams(
						seedmms,   // # mismatches allowed in seed
						seedlen,   // length of seed
						seedival,  // interval between seeds
						minsc,     // minimum score for valid alignment
						floorsc);  // local-alignment floor score
					
					if(reportImmediately) {
						assert(msink != NULL);
						assert(res->repOk());
						// Check that alignment accurately reflects the
						// reference characters aligned to
						assert(res->alres.matchesRef(
							rd,
							ref,
							tmp_rf_,
							tmp_rdseq_,
							tmp_qseq_,
							raw_refbuf_,
							raw_destU32_,
							raw_matches_));
						// Report an unpaired alignment
						assert(!msink->maxed());
						if(msink->report(
							0,
							mate1 ? &res->alres : NULL,
							mate1 ? NULL : &res->alres))
						{
							// Short-circuited because a limit, e.g. -k, -m or
							// -M, was exceeded
							//IF_TIMER_END() {
							//	cerr << "Saw a long extendSeeds (" << total_usecs << ")" << endl;
							//}
							return true;
						}
						if(tighten > 0 &&
						   msink->Mmode() &&
						   msink->hasSecondBestUnp1())
						{
							if(tighten == 1) {
								if(msink->bestUnp1() >= minsc) {
									minsc = msink->bestUnp1();
									if(minsc < perfectScore &&
									   msink->bestUnp1() == msink->secondBestUnp1())
									{
										minsc++;
									}
								}
							} else {
								if(msink->secondBestUnp1() >= minsc) {
									minsc = msink->secondBestUnp1();
									if(minsc < perfectScore) {
										minsc++;
									}
								}
							}
							assert_leq(minsc, perfectScore);
						}
					}
				}

				// At this point we know that we aren't bailing, and will
				// continue to resolve seed hits.  

			} // while(!gws_[i].done())
		}
	}
	// Short-circuited because a limit, e.g. -k, -m or -M, was exceeded
	//IF_TIMER_END() {
	//	cerr << "Saw a long extendSeeds (" << total_usecs << ")" << endl;
	//}
	return false;
}

/**
 * Given a read, perform full dynamic programming against the entire
 * reference.  Optionally report alignments to a AlnSinkWrap object
 * as they are discovered.
 *
 * If 'reportImmediately' is true, returns true iff a call to
 * msink->report() returned true (indicating that the reporting
 * policy is satisfied and we can stop).  Otherwise, returns false.
 */
bool SwDriver::sw(
	const Read& rd,              // read to align
	bool color,                  // true -> read is colorspace
	const BitPairReference& ref, // Reference strings
	SwAligner& swa,              // dynamic programming aligner
	const Scoring& sc,           // scoring scheme
	TAlScore minsc,              // minimum score for valid alignment
	TAlScore floorsc,            // local-alignment floor score
	RandomSource& rnd,           // pseudo-random source
	SwMetrics& swm,              // dynamic programming metrics
	AlnSinkWrap* msink,          // HitSink for multiseed-style aligner
	bool reportImmediately,      // whether to report hits immediately to msink
	EList<SwCounterSink*>* swCounterSinks, // send counter updates to these
	EList<SwActionSink*>* swActionSinks)   // send action-list updates to these
{
	assert(!reportImmediately || msink != NULL);
	return false;
}

/**
 * Given a collection of SeedHits for both mates in a read pair, extend seed
 * alignments into full alignments and then look for the opposite mate using
 * dynamic programming.  Where possible, try to avoid redundant offset lookups.
 * Optionally report alignments to a AlnSinkWrap object as they are discovered.
 *
 * If 'reportImmediately' is true, returns true iff a call to
 * msink->report() returned true (indicating that the reporting
 * policy is satisfied and we can stop).  Otherwise, returns false.
 *
 * REDUNDANT SEED HITS
 *
 * See notes at top of aligner_sw_driver.h.
 *
 * REDUNDANT ALIGNMENTS
 *
 * See notes at top of aligner_sw_driver.h.
 *
 * MIXING PAIRED AND UNPAIRED ALIGNMENTS
 *
 * There are distinct paired-end alignment modes for the cases where (a) the
 * user does or does not want to see unpaired alignments for individual mates
 * when there are no reportable paired-end alignments involving both mates, and
 * (b) the user does or does not want to see discordant paired-end alignments.
 * The modes have implications for this function and for the AlnSinkWrap, since
 * it affects when we're "done."  Also, whether the user has asked us to report
 * discordant alignments affects whether and how much searching for unpaired
 * alignments we must do (i.e. if there are no paired-end alignments, we must
 * at least do -m 1 for both mates).
 *
 * Mode 1: Just concordant paired-end.  Print only concordant paired-end
 * alignments.  As soon as any limits (-k/-m/-M) are reached, stop.
 *
 * Mode 2: Concordant and discordant paired-end.  If -k/-m/-M limits are
 * reached for paired-end alignments, stop.  Otherwise, if no paired-end
 * alignments are found, align both mates in an unpaired -m 1 fashion.  If
 * there is exactly one unpaired alignment for each mate, report the
 * combination as a discordant alignment.
 *
 * Mode 3: Concordant paired-end if possible, otherwise unpaired.  If -k/-M
 * limit is reached for paired-end alignmnts, stop.  If -m limit is reached for
 * paired-end alignments or no paired-end alignments are found, align both
 * mates in an unpaired fashion.  All the same settings governing validity and
 * reportability in paired-end mode apply here too (-k/-m/-M/etc).
 *
 * Mode 4: Concordant or discordant paired-end if possible, otherwise unpaired.
 * If -k/-M limit is reached for paired-end alignmnts, stop.  If -m limit is
 * reached for paired-end alignments or no paired-end alignments are found,
 * align both mates in an unpaired fashion.  If the -m limit was reached, there
 * is no need to search for a discordant alignment, and unapired alignment can
 * proceed as in Mode 3.  If no paired-end alignments were found, then unpaired
 * alignment proceeds as in Mode 3 but with this caveat: alignment must be at
 * least as thorough as dictated by -m 1 up until the point where
 *
 * Print paired-end alignments when there are reportable paired-end
 * alignments, otherwise report reportable unpaired alignments.  If -k limit is
 * reached for paired-end alignments, stop.  If -m/-M limit is reached for
 * paired-end alignments, stop searching for paired-end alignments and look
 * only for unpaired alignments.  If searching only for unpaired alignments,
 * respect -k/-m/-M limits separately for both mates.
 *
 * The return value from the AlnSinkWrap's report member function must be
 * specific enough to distinguish between:
 *
 * 1. Stop searching for paired-end alignments
 * 2. Stop searching for alignments for unpaired alignments for mate #1
 * 3. Stop searching for alignments for unpaired alignments for mate #2
 * 4. Stop searching for any alignments
 *
 * Note that in Mode 2, options affecting validity and reportability of
 * alignments apply .  E.g. if -m 1 is specified
 *
 * WORKFLOW
 *
 * Our general approach to finding paired and unpaired alignments here
 * is as follows:
 *
 * - For mate in mate1, mate2:
 *   - For each seed hit in mate:
 *     - Try to extend it into a full alignment; if we can't, continue
 *       to the next seed hit
 *     - Look for alignment for opposite mate; if we can't find one,
 *     - 
 *     - 
 *
 */
bool SwDriver::extendSeedsPaired(
	const Read& rd,              // mate to align as anchor
	const Read& ord,             // mate to align as opposite
	bool anchor1,                // true iff anchor mate is mate1
	bool oppFilt,                // true iff opposite mate was filtered out
	bool color,                  // true -> reads are colorspace
	SeedResults& sh,             // seed hits for anchor
	const Ebwt& ebwt,            // BWT
	const BitPairReference& ref, // Reference strings
	SwAligner& swa,              // dynamic programming aligner for anchor
	SwAligner& oswa,             // dynamic programming aligner for opposite
	const Scoring& sc,           // scoring scheme
	const PairedEndPolicy& pepol,// paired-end policy
	int seedmms,                 // # mismatches allowed in seed
	int seedlen,                 // length of seed
	int seedival,                // interval between seeds
	TAlScore minsc,              // minimum score for valid anchor aln
	TAlScore ominsc,             // minimum score for valid opposite aln
	TAlScore floorsc,            // local-alignment score floor for anchor
	TAlScore ofloorsc,           // local-alignment score floor for opposite
	int nceil,                   // max # Ns permitted in ref for anchor
	int onceil,                  // max # Ns permitted in ref for opposite
	bool nofw,                   // don't align forward read
	bool norc,                   // don't align revcomp read
	const SimpleFunc& maxeltf,   // # elts to explore as function of total elts
	size_t maxhalf,              // max width in either direction for DP tables
	bool doUngapped,             // do ungapped alignment
	size_t ungappedThresh,       // all attempts after this many are ungapped
	bool enable8,                // use 8-bit SSE where possible
	bool refscan,                // use reference scanning
	int tighten,                 // -M score tightening mode
	AlignmentCacheIface& ca,     // alignment cache for seed hits
	RandomSource& rnd,           // pseudo-random source
	WalkMetrics& wlm,            // group walk left metrics
	SwMetrics& swmSeed,          // DP metrics for seed-extend
	SwMetrics& swmMate,          // DP metrics for mate finidng
	AlnSinkWrap* msink,          // AlnSink wrapper for multiseed-style aligner
	bool swMateImmediately,      // whether to look for mate immediately
	bool reportImmediately,      // whether to report hits immediately to msink
	bool discord,                // look for discordant alignments?
	bool mixed,                  // look for unpaired as well as paired alns?
	bool& exhaustive)
{
	typedef std::pair<uint32_t, uint32_t> U32Pair;

	assert(!reportImmediately || msink != NULL);
	assert(!reportImmediately || !msink->maxed());
	assert(!msink->state().doneWithMate(anchor1));
	assert_gt(seedlen, 0);

	assert_geq(nceil, 0);
	assert_geq(onceil, 0);
	assert_leq((size_t)nceil,  rd.length());
	assert_leq((size_t)onceil, ord.length());

	const size_t rdlen  = rd.length();
	const size_t ordlen = ord.length();
	const TAlScore perfectScore = sc.perfectScore(rdlen);
	const TAlScore operfectScore = sc.perfectScore(ordlen);

	assert_leq(minsc, perfectScore);
	assert(oppFilt || ominsc <= operfectScore);

	TAlScore bestPairScore = perfectScore + operfectScore;
	if(tighten > 0 && msink->Mmode() && msink->hasSecondBestPair()) {
		// Paired-end alignments should have at least this score from now
		TAlScore ps = ((tighten == 1) ? msink->bestPair() : msink->secondBestPair());
		if(tighten == 1 && ps < bestPairScore &&
		   msink->bestPair() == msink->secondBestPair())
		{
			ps++;
		}
		if(tighten == 2 && ps < bestPairScore) {
			ps++;
		}
		// Anchor mate must have score at least 'ps' minus the best possible
		// score for the opposite mate.
		TAlScore nc = ps - operfectScore;
		if(nc > minsc) {
			minsc = nc;
		}
		assert_leq(minsc, perfectScore);
	}

	DynProgFramer dpframe(!gReportOverhangs);
	swa.reset();
	oswa.reset();

	// Initialize a set of GroupWalks, one for each seed.  Also, intialize the
	// accompanying lists of reference seed hits (satups*) and the combiners
	// that link the reference-scanning results to the BW walking results
	// (sacomb_).  Finally, initialize the SeedScanTable and SeedScanner
	// objects that will actually do the reference scanning and opportunistic
	// resolution of offsets.
	const size_t nsm = 3;
	const size_t nonz = sh.nonzeroOffsets(); // non-zero positions
	assert_gt(nonz, 0);
	double maxelt_db = maxeltf.f<double>((double)nonz);
	if(maxelt_db < std::numeric_limits<double>::max()) {
		maxelt_db *= 32.0/pow((double)max<size_t>(rdlen, 100), 0.75);
		maxelt_db = max<double>(maxelt_db, 2.0f);
	}
	size_t maxelt = (size_t)maxelt_db;
	if(maxelt_db == std::numeric_limits<double>::max()) {
		maxelt = std::numeric_limits<size_t>::max();
	}
	
	size_t eeHits = sh.numE2eHits();
	bool eeMode = eeHits > 0;
	bool firstEe = true;
	bool firstExtend = true;

	size_t nelt = 0, neltLeft = 0;
	const size_t rows = rdlen + (color ? 1 : 0);
	const size_t orows  = ordlen + (color ? 1 : 0);
	size_t eltsDone = 0;
	while(true) {
		if(eeMode) {
			if(firstEe) {
				firstEe = false;
				eeMode = eeSaTups(
					sh,           // seed hits to extend into full alignments
					ebwt,         // BWT
					ref,          // Reference strings
					rnd,          // pseudo-random generator
					wlm,          // group walk left metrics
					swmSeed,      // seed-extend metrics
					nelt);        // out: # elements total
				assert_eq(gws_.size(), rands_.size());
				assert_eq(gws_.size(), satpos_.size());
				neltLeft = nelt;
			} else {
				eeMode = false;
			}
		}
		if(!eeMode) {
			if(minsc == perfectScore) {
				// Already found all perfect hits!
				return false;
			}
			if(firstExtend) {
				nelt = 0;
				prioritizeSATups(
					sh,           // seed hits to extend into full alignments
					ebwt,         // BWT
					ref,          // Reference strings
					refscan,      // do reference scanning?
					maxelt,       // max rows to consider per position
					nsm,          // smallness threshold
					ca,           // alignment cache for seed hits
					rnd,          // pseudo-random generator
					wlm,          // group walk left metrics
					nelt);        // out: # elements total
				assert_eq(gws_.size(), rands_.size());
				assert_eq(gws_.size(), satpos_.size());
				neltLeft = nelt;
				firstExtend = false;
			}
			if(!(neltLeft > 0 && eltsDone < maxelt)) {
				// Finished examining gapped candidates
				break;
			}
		}
		// neltLeft is initialized separately, once for the end-to-end hits and
		// once for the seed hits.  eltsDone and maxelt are initialized once
		// and carried over across end-to-end & seed modes.
		for(size_t i = 0; i < gws_.size(); i++) {
			if(eeMode && eehits_[i].score < minsc) {
				break;
			}
			bool small = satpos_[i].sat.size() < nsm;
			bool fw          = satpos_[i].pos.fw;
			uint32_t rdoff   = satpos_[i].pos.rdoff;
			uint32_t seedlen = satpos_[i].pos.seedlen;
			if(!fw) {
				// 'rdoff' and 'offidx' are with respect to the 5' end of
				// the read.  Here we convert rdoff to be with respect to
				// the upstream (3') end of ther read.
				rdoff = (uint32_t)(rdlen - rdoff - seedlen);
			}
			bool first = true;
			assert_leq(eltsDone, maxelt);
			// If the range is small, investigate all elements now.  If the
			// range is large, just investigate one and move on - we might come
			// back to this range later.
			while(!rands_[i].done() &&
			      (eltsDone < maxelt && (first || small)))
			{
				if(minsc == perfectScore) {
					if(!eeMode || eehits_[i].score < perfectScore) {
						return false;
					}
				} else if(eeMode && eehits_[i].score < minsc) {
					break;
				}
				first = false;
				assert(!gws_[i].done());
				// Resolve next element offset
				WalkResult wr;
				uint32_t elt = rands_[i].next(rnd);
				gws_[i].advanceElement(elt, wr, wlm);
				eltsDone++;
				assert_gt(neltLeft, 0);
				neltLeft--;
				assert_neq(0xffffffff, wr.toff);
				uint32_t tidx = 0, toff = 0, tlen = 0;
				ebwt.joinedToTextOff(
					wr.elt.len,
					wr.toff,
					tidx,
					toff,
					tlen);
				tlen += (color ? 1 : 0);
				if(tidx == 0xffffffff) {
					// The seed hit straddled a reference boundary so the seed hit
					// isn't valid
					continue;
				}
#ifndef NDEBUG
				if(!eeMode) { // Check that seed hit matches reference
					uint64_t key = satpos_[i].sat.key.seq;
					for(size_t k = 0; k < wr.elt.len; k++) {
						int c = ref.getBase(tidx, toff + wr.elt.len - k - 1);
						int ck = (int)(key & 3);
						key >>= 2;
						assert_eq(c, ck);
					}
				}
#endif
				// Find offset of alignment's upstream base assuming net gaps=0
				// between beginning of read and beginning of seed hit
				int64_t refoff = (int64_t)toff - rdoff;
				EIvalMergeList& seenDiags  = anchor1 ? seenDiags1_ : seenDiags2_;
				// Coordinate of the seed hit w/r/t the pasted reference string
				Coord refcoord(tidx, refoff, fw);
				if(seenDiags.locusPresent(refcoord)) {
					// Already handled alignments seeded on this diagonal
					swmSeed.rshit++;
					continue;
				}
				// Now that we have a seed hit, there are many issues to solve
				// before we have a completely framed dynamic programming problem.
				// They include:
				//
				// 1. Setting reference offsets on either side of the seed hit,
				//    accounting for where the seed occurs in the read
				// 2. Adjusting the width of the banded dynamic programming problem
				//    and adjusting reference bounds to allow for gaps in the
				//    alignment
				// 3. Accounting for the edges of the reference, which can impact
				//    the width of the DP problem and reference bounds.
				// 4. Perhaps filtering the problem down to a smaller problem based
				//    on what DPs we've already solved for this read
				//
				// We do #1 here, since it is simple and we have all the seed-hit
				// information here.  #2 and #3 are handled in the DynProgFramer.
				int readGaps = 0, refGaps = 0;
				bool ungapped = false;
				if(!eeMode) {
					readGaps = sc.maxReadGaps(minsc, rdlen);
					refGaps  = sc.maxRefGaps(minsc, rdlen);
					ungapped = (readGaps == 0 && refGaps == 0) || eltsDone >= ungappedThresh;
				}
				int state = FOUND_NONE;
				bool found = false;
				if(eeMode) {
					resEe_.reset();
					resEe_.alres.reset();
					const EEHit& h = eehits_[i];
					assert_leq(h.score, perfectScore);
					resEe_.alres.setScore(AlnScore(h.score, h.ns(), 0));
					resEe_.alres.setShape(
						refcoord.ref(),  // ref id
						refcoord.off(),  // 0-based ref offset
						fw,              // aligned to Watson?
						rdlen,           // read length
						false,           // read was colorspace?
						true,            // pretrim soft?
						0,               // pretrim 5' end
						0,               // pretrim 3' end
						true,            // alignment trim soft?
						0,               // alignment trim 5' end
						0);              // alignment trim 3' end
					resEe_.alres.setRefNs(h.refns());
					if(h.mms() > 0) {
						assert_eq(1, h.mms());
						resEe_.alres.ned().push_back(h.e1);
					}
					state = FOUND_EE;
					found = true;
					Interval refival(refcoord, 1);
					seenDiags.add(refival);
				} else if(doUngapped && ungapped) {
					resUngap_.reset();
					int al = swa.ungappedAlign(
						fw ? rd.patFw : rd.patRc,
						fw ? rd.qual  : rd.qualRev,
						refcoord,
						ref,
						tlen,
						sc,
						gReportOverhangs,
						minsc,
						floorsc,
						resUngap_);
					Interval refival(refcoord, 1);
					seenDiags.add(refival);
					if(al == 0) {
						swmSeed.ungapfail++;
						continue;
					} else if(al == -1) {
						swmSeed.ungapnodec++;
					} else {
						found = true;
						state = FOUND_UNGAPPED;
						swmSeed.ungapsucc++;
					}
				}
				int64_t pastedRefoff = (int64_t)wr.toff - rdoff;
				DPRect rect;
				if(state == FOUND_NONE) {
					found = dpframe.frameSeedExtensionRect(
						refoff,   // ref offset implied by seed hit assuming no gaps
						rows,     // length of read sequence used in DP table (so len
								  // of +1 nucleotide sequence for colorspace reads)
						tlen,     // length of reference
						readGaps, // max # of read gaps permitted in opp mate alignment
						refGaps,  // max # of ref gaps permitted in opp mate alignment
						(size_t)nceil, // # Ns permitted
						maxhalf,  // max width in either direction
						rect);    // DP rectangle
					assert(rect.repOk());
					// Add the seed diagonal at least
					seenDiags.add(Interval(refcoord, 1));
					if(!found) {
						continue;
					}
				}
				int64_t leftShift = refoff - rect.refl;
				size_t nwindow = 0;
				if(toff >= rect.refl) {
					nwindow = (size_t)(toff - rect.refl);
				}
				// NOTE: We might be taking off more than we should because the
				// pasted string omits non-A/C/G/T characters, but we included them
				// when calculating leftShift.  We'll account for this later.
				pastedRefoff -= leftShift;
				size_t nsInLeftShift = 0;
				if(state == FOUND_NONE) {
					sscan_.init(sstab_);
					if(!swa.initedRead()) {
						// Initialize the aligner with a new read
						swa.initRead(
							rd.patFw,  // fw version of query
							rd.patRc,  // rc version of query
							rd.qual,   // fw version of qualities
							rd.qualRev,// rc version of qualities
							0,         // off of first char in 'rd' to consider
							rdlen,     // off of last char (excl) in 'rd' to consider
							color,     // colorspace?
							sc,        // scoring scheme
							floorsc);  // local-alignment floor score
					}
					swa.initRef(
						fw,        // whether to align forward or revcomp read
						tidx,      // reference aligned against
						rect,      // DP rectangle
						ref,       // Reference strings
						tlen,      // length of reference sequence
						sc,        // scoring scheme
						minsc,     // minimum score permitted
						enable8,   // use 8-bit SSE if possible?
						true,      // this is a seed extension - not finding a mate
						&sscan_,   // reference scanner for resolving offsets
						nwindow,
						nsInLeftShift);
					if(refscan) {
						// Take reference-scanner hits and turn them into offset
						// resolutions.
						wlm.refscanhits += sscan_.hits().size();
						pastedRefoff += nsInLeftShift;
						for(size_t j = 0; j < sscan_.hits().size(); j++) {
							// Get identifier for the appropriate combiner
							U32Pair id = sscan_.hits()[j].id();
							// Get the hit's offset in pasted-reference coordinates
							int64_t off = sscan_.hits()[j].off() + pastedRefoff;
							assert_geq(off, sscan_.hits()[j].ns());
							off -= sscan_.hits()[j].ns();
							assert_geq(off, 0);
							assert_lt(off, (int64_t)ebwt.eh().lenNucs());
							assert_lt(off, (int64_t)0xffffffff);
							// Check that reference sequence actually matches seed
#ifndef NDEBUG
							uint32_t tidx2 = 0, toff2 = 0, tlen2 = 0;
							ebwt.joinedToTextOff(
								wr.elt.len,
								(uint32_t)off,
								tidx2,
								toff2,
								tlen2);
							assert_neq(0xffffffff, tidx2);
							uint64_t key = sacomb_[id.first].satup().key.seq;
							for(size_t k = 0; k < wr.elt.len; k++) {
								int c = ref.getBase(tidx2, toff2 + wr.elt.len - k - 1);
								int ck = (int)(key & 3);
								key >>= 2;
								assert_eq(c, ck);
							}
#endif
							// Install it
							if(sacomb_[id.first].addRefscan((uint32_t)off)) {
								// It was new; see if it leads to any resolutions
								sacomb_[id.first].tryResolving(wlm.refresolves);
							}
						}
					}
					// Because of how we framed the problem, we can say that we've
					// exhaustively scored the seed diagonal as well as maxgaps
					// diagonals on either side
					Interval refival(tidx, 0, fw, 0);
					rect.initIval(refival);
					seenDiags.add(refival);
					// Now fill the dynamic programming matrix and return true iff
					// there is at least one valid alignment
					found = swa.align(rnd);
					if(!found) {
						continue; // Look for more anchor alignments
					}
				}
				bool firstInner = true;
				while(true) {
					assert(found);
					SwResult *res = NULL;
					if(state == FOUND_EE) {
						if(!firstInner) {
							break;
						}
						res = &resEe_;
					} else if(state == FOUND_UNGAPPED) {
						if(!firstInner) {
							break;
						}
						res = &resUngap_;
					} else {
						resGap_.reset();
						assert(resGap_.empty());
						if(swa.done()) {
							break;
						}
						swa.nextAlignment(resGap_, minsc, rnd);
						found = !resGap_.empty();
						if(!found) {
							break;
						}
						res = &resGap_;
					}
					assert(res != NULL);
					firstInner = false;
					assert(res->alres.matchesRef(
						rd,
						ref,
						tmp_rf_,
						tmp_rdseq_,
						tmp_qseq_,
						raw_refbuf_,
						raw_destU32_,
						raw_matches_));
					Interval refival(tidx, 0, fw, tlen);
					assert_gt(res->alres.refExtent(), 0);
					if(gReportOverhangs &&
					   !refival.containsIgnoreOrient(res->alres.refival()))
					{
						res->alres.clipOutside(true, 0, tlen);
						if(res->alres.refExtent() == 0) {
							continue;
						}
					}
					assert(gReportOverhangs ||
					       refival.containsIgnoreOrient(res->alres.refival()));
					// Did the alignment fall entirely outside the reference?
					if(!refival.overlapsIgnoreOrient(res->alres.refival())) {
						continue;
					}
					// Is this alignment redundant with one we've seen previously?
					if(redAnchor_.overlap(res->alres)) {
						continue;
					}
					redAnchor_.add(res->alres);
					// Annotate the AlnRes object with some key parameters
					// that were used to obtain the alignment.
					res->alres.setParams(
						seedmms,   // # mismatches allowed in seed
						seedlen,   // length of seed
						seedival,  // interval between seeds
						minsc,     // minimum score for valid alignment
						floorsc);  // local-alignment floor score
					bool foundMate = false;
					TRefOff off = res->alres.refoff();
					if( msink->state().doneWithMate(!anchor1) &&
					   !msink->state().doneWithMate( anchor1))
					{
						// We're done with the opposite mate but not with the
						// anchor mate; don't try to mate up the anchor.
						swMateImmediately = false;
					}
					if(found && swMateImmediately) {
						assert(!msink->state().doneWithMate(!anchor1));
						bool oleft = false, ofw = false;
						int64_t oll = 0, olr = 0, orl = 0, orr = 0;
						assert(!msink->state().done());
						foundMate = !oppFilt;
						TAlScore ominsc_cur = ominsc;
						bool oungapped = false;
						int oreadGaps = 0, orefGaps = 0;
						//int oungappedAlign = -1; // defer
						if(foundMate) {
							// Adjust ominsc given the alignment score of the
							// anchor mate
							ominsc_cur = ominsc;
							if(tighten > 0 && msink->Mmode() && msink->hasSecondBestPair()) {
								// Paired-end alignments should have at least this score from now
								TAlScore ps = ((tighten == 1) ? msink->bestPair() : msink->secondBestPair());
								if(tighten == 1 && ps < bestPairScore &&
								   msink->bestPair() == msink->secondBestPair())
								{
									ps++;
								}
								if(tighten == 2 && ps < bestPairScore) {
									ps++;
								}
								// Anchor mate must have score at least 'ps' minus the best possible
								// score for the opposite mate.
								TAlScore nc = ps - res->alres.score().score();
								if(nc > ominsc_cur) {
									ominsc_cur = nc;
									assert_leq(ominsc_cur, operfectScore);
								}
							}
							oreadGaps = sc.maxReadGaps(ominsc_cur, ordlen);
							orefGaps  = sc.maxRefGaps (ominsc_cur, ordlen);
							oungapped = (oreadGaps == 0 && orefGaps == 0);
							// TODO: Something lighter-weight than DP to scan
							// for other mate??
							//if(oungapped) {
							//	oresUngap_.reset();
							//	oungappedAlign = oswa.ungappedAlign(
							//		ofw ? ord.patFw : ord.patRc,
							//		ofw ? ord.qual  : ord.qualRev,
							//		orefcoord,
							//		ref,
							//		otlen,
							//		sc,
							//		gReportOverhangs,
							//		ominsc_cur,
							//		floorsc,
							//		oresUngap_);
							//}
							foundMate = pepol.otherMate(
								anchor1,             // anchor mate is mate #1?
								fw,                  // anchor aligned to Watson?
								off,                 // offset of anchor mate
								orows + oreadGaps,   // max # columns spanned by alignment
								tlen,                // reference length
								anchor1 ? rd.length() : ord.length(), // mate 1 len
								anchor1 ? ord.length() : rd.length(), // mate 2 len
								oleft,               // out: look left for opposite mate?
								oll,
								olr,
								orl,
								orr,
								ofw);
						}
						DPRect orect;
						if(foundMate) {
							foundMate = dpframe.frameFindMateRect(
								!oleft,      // true iff anchor alignment is to the left
								oll,         // leftmost Watson off for LHS of opp aln
								olr,         // rightmost Watson off for LHS of opp aln
								orl,         // leftmost Watson off for RHS of opp aln
								orr,         // rightmost Watson off for RHS of opp aln
								orows,       // length of opposite mate
								tlen,        // length of reference sequence aligned to
								oreadGaps,   // max # of read gaps in opp mate aln
								orefGaps,    // max # of ref gaps in opp mate aln
								(size_t)onceil, // max # Ns on opp mate
								maxhalf,     // max width in either direction
								orect);      // DP rectangle
							assert(!foundMate || orect.refr >= orect.refl);
						}
						if(foundMate) {
							oresGap_.reset();
							assert(oresGap_.empty());
							if(!oswa.initedRead()) {
								oswa.initRead(
									ord.patFw,  // read to align
									ord.patRc,  // qualities
									ord.qual,   // read to align
									ord.qualRev,// qualities
									0,          // off of first char to consider
									ordlen,     // off of last char (ex) to consider
									color,      // colorspace?
									sc,         // scoring scheme
									ofloorsc);  // local-alignment floor score
							}
							// Given the boundaries defined by refi and reff, initilize
							// the SwAligner with the dynamic programming problem that
							// aligns the read to this reference stretch.
							size_t onsInLeftShift = 0;
							assert_geq(orect.refr, orect.refl);
							oswa.initRef(
								ofw,       // align forward or revcomp read?
								tidx,      // reference aligned against
								orect,     // DP rectangle
								ref,       // Reference strings
								tlen,      // length of reference sequence
								sc,        // scoring scheme
								ominsc_cur,// min score for valid alignments
								enable8,   // use 8-bit SSE if possible?
								false,     // this is finding a mate - not seed ext
								NULL,      // TODO: scan w/r/t other SeedResults
								0,         // nwindow?
								onsInLeftShift);
							// TODO: Can't we add some diagonals to the
							// opposite mate's seenDiags when we fill in the
							// opposite mate's DP?  Or can we?  We might want
							// to use this again as an anchor - will that still
							// happen?  Also, isn't there a problem with
							// consistency of the minimum score?  Minimum score
							// here depends in part on the score of the anchor
							// alignment here, but it won't when the current
							// opposite becomes the anchor.
							
							// Because of how we framed the problem, we can say
							// that we've exhaustively explored the "core"
							// diagonals
							//Interval orefival(tidx, 0, ofw, 0);
							//orect.initIval(orefival);
							//oseenDiags.add(orefival);

							// Now fill the dynamic programming matrix, return true
							// iff there is at least one valid alignment
							foundMate = oswa.align(rnd);
						}
						bool didAnchor = false;
						do {
							oresGap_.reset();
							assert(oresGap_.empty());
							if(foundMate && oswa.done()) {
								foundMate = false;
							} else if(foundMate) {
								oswa.nextAlignment(oresGap_, ominsc_cur, rnd);
								foundMate = !oresGap_.empty();
								assert(!foundMate || oresGap_.alres.matchesRef(
									ord,
									ref,
									tmp_rf_,
									tmp_rdseq_,
									tmp_qseq_,
									raw_refbuf_,
									raw_destU32_,
									raw_matches_));
							}
							if(foundMate) {
								// Redundant with one we've seen previously?
								if(!redAnchor_.overlap(oresGap_.alres)) {
									redAnchor_.add(oresGap_.alres);
								}
								assert_eq(ofw, oresGap_.alres.fw());
								// Annotate the AlnRes object with some key parameters
								// that were used to obtain the alignment.
								oresGap_.alres.setParams(
									seedmms,    // # mismatches allowed in seed
									seedlen,    // length of seed
									seedival,   // interval between seeds
									ominsc,     // minimum score for valid alignment
									ofloorsc);  // local-alignment floor score
								assert_gt(oresGap_.alres.refExtent(), 0);
								if(gReportOverhangs &&
								   !refival.containsIgnoreOrient(oresGap_.alres.refival()))
								{
									oresGap_.alres.clipOutside(true, 0, tlen);
									foundMate = oresGap_.alres.refExtent() > 0;
								}
								if(foundMate && 
								   ((!gReportOverhangs &&
									 !refival.containsIgnoreOrient(oresGap_.alres.refival())) ||
									 !refival.overlapsIgnoreOrient(oresGap_.alres.refival())))
								{
									foundMate = false;
								}
							}
							TRefId refid;
							TRefOff off1, off2;
							TRefOff fragoff;
							size_t len1, len2, fraglen;
							bool fw1, fw2;
							int pairCl = PE_ALS_DISCORD;
							if(foundMate) {
								refid = res->alres.refid();
								assert_eq(refid, oresGap_.alres.refid());
								off1 = anchor1 ? off : oresGap_.alres.refoff();
								off2 = anchor1 ? oresGap_.alres.refoff() : off;
								len1 = anchor1 ?
									res->alres.refExtent() : oresGap_.alres.refExtent();
								len2 = anchor1 ?
									oresGap_.alres.refExtent() : res->alres.refExtent();
								fw1  = anchor1 ? res->alres.fw() : oresGap_.alres.fw();
								fw2  = anchor1 ? oresGap_.alres.fw() : res->alres.fw();
								fragoff = min<TRefOff>(off1, off2);
								fraglen = (size_t)max<TRefOff>(
									off1 - fragoff + (TRefOff)len1,
									off2 - fragoff + (TRefOff)len2);
								// Check that final mate alignments are consistent with
								// paired-end fragment constraints
								pairCl = pepol.peClassifyPair(
									off1,
									len1,
									fw1,
									off2,
									len2,
									fw2);
								// Instead of trying
								//foundMate = pairCl != PE_ALS_DISCORD;
							}
							if(msink->state().doneConcordant()) {
								foundMate = false;
							}
							if(reportImmediately) {
								if(foundMate) {
									// Report pair to the AlnSinkWrap
									assert(!msink->state().doneConcordant());
									assert(msink != NULL);
									assert(res->repOk());
									assert(oresGap_.repOk());
									// Report an unpaired alignment
									assert(!msink->maxed());
									assert(!msink->state().done());
									bool doneUnpaired = false;
									if(mixed || discord) {
										// Report alignment for mate #1 as an
										// unpaired alignment.
										if(!anchor1 || !didAnchor) {
											if(anchor1) {
												didAnchor = true;
											}
											const AlnRes& r1 = anchor1 ?
												res->alres : oresGap_.alres;
											if(!redMate1_.overlap(r1)) {
												redMate1_.add(r1);
												if(msink->report(0, &r1, NULL)) {
													doneUnpaired = true; // Short-circuited
												}
											}
										}
										// Report alignment for mate #2 as an
										// unpaired alignment.
										if(anchor1 || !didAnchor) {
											if(!anchor1) {
												didAnchor = true;
											}
											const AlnRes& r2 = anchor1 ?
												oresGap_.alres : res->alres;
											if(!redMate2_.overlap(r2)) {
												redMate2_.add(r2);
												if(msink->report(0, NULL, &r2)) {
													doneUnpaired = true; // Short-circuited
												}
											}
										}
									} // if(mixed || discord)
									bool donePaired = false;
									if(pairCl != PE_ALS_DISCORD) {
										if(msink->report(
										       0,
										       anchor1 ? &res->alres : &oresGap_.alres,
										       anchor1 ? &oresGap_.alres : &res->alres))
										{
											// Short-circuited because a limit, e.g.
											// -k, -m or -M, was exceeded
											donePaired = true;
										} else {
											if(tighten > 0 && msink->Mmode() && msink->hasSecondBestPair()) {
												// Paired-end alignments should have at least this score from now
												TAlScore ps = ((tighten == 1) ? msink->bestPair() : msink->secondBestPair());
												if(tighten == 1 && ps < bestPairScore &&
												   msink->bestPair() == msink->secondBestPair())
												{
													ps++;
												}
												if(tighten == 2 && ps < bestPairScore) {
													ps++;
												}
												// Anchor mate must have score at least 'ps' minus the best possible
												// score for the opposite mate.
												TAlScore nc = ps - operfectScore;
												if(nc > minsc) {
													minsc = nc;
													assert_leq(minsc, perfectScore);
													if(minsc > res->alres.score().score()) {
														// We're done with this anchor
														break;
													}
												}
												assert_leq(minsc, perfectScore);
											}
										}
									} // if(pairCl != PE_ALS_DISCORD)
									if(donePaired || doneUnpaired) {
										return true;
									}
									if(msink->state().doneWithMate(anchor1)) {
										// We're now done with the mate that we're
										// currently using as our anchor.  We're not
										// with the read overall.
										return false;
									}
								} else if((mixed || discord) && !didAnchor) {
									didAnchor = true;
									// Report unpaired hit for anchor
									assert(msink != NULL);
									assert(res->repOk());
									// Check that alignment accurately reflects the
									// reference characters aligned to
									assert(res->alres.matchesRef(
										rd,
										ref,
										tmp_rf_,
										tmp_rdseq_,
										tmp_qseq_,
										raw_refbuf_,
										raw_destU32_,
										raw_matches_));
									// Report an unpaired alignment
									assert(!msink->maxed());
									assert(!msink->state().done());
									// Report alignment for mate #1 as an
									// unpaired alignment.
									if(!msink->state().doneUnpaired(anchor1)) {
										const AlnRes& r = res->alres;
										RedundantAlns& red = anchor1 ? redMate1_ : redMate2_;
										const AlnRes* r1 = anchor1 ? &res->alres : NULL;
										const AlnRes* r2 = anchor1 ? NULL : &res->alres;
										if(!red.overlap(r)) {
											red.add(r);
											if(msink->report(0, r1, r2)) {
												return true; // Short-circuited
											}
										}
									}
									if(msink->state().doneWithMate(anchor1)) {
										// Done with mate, but not read overall
										return false;
									}
								}
							}
						} while(!oresGap_.empty());
					} // if(found && swMateImmediately)
					else if(found) {
						assert(!msink->state().doneWithMate(anchor1));
						// We found an anchor alignment but did not attempt to find
						// an alignment for the opposite mate (probably because
						// we're done with it)
						if(reportImmediately && (mixed || discord)) {
							// Report unpaired hit for anchor
							assert(msink != NULL);
							assert(res->repOk());
							// Check that alignment accurately reflects the
							// reference characters aligned to
							assert(res->alres.matchesRef(
								rd,
								ref,
								tmp_rf_,
								tmp_rdseq_,
								tmp_qseq_,
								raw_refbuf_,
								raw_destU32_,
								raw_matches_));
							// Report an unpaired alignment
							assert(!msink->maxed());
							assert(!msink->state().done());
							// Report alignment for mate #1 as an
							// unpaired alignment.
							if(!msink->state().doneUnpaired(anchor1)) {
								const AlnRes& r = res->alres;
								RedundantAlns& red = anchor1 ? redMate1_ : redMate2_;
								const AlnRes* r1 = anchor1 ? &res->alres : NULL;
								const AlnRes* r2 = anchor1 ? NULL : &res->alres;
								if(!red.overlap(r)) {
									red.add(r);
									if(msink->report(0, r1, r2)) {
										return true; // Short-circuited
									}
								}
							}
							if(msink->state().doneWithMate(anchor1)) {
								// Done with mate, but not read overall
								return false;
							}
						}
					}
				} // while(true)
				
				// At this point we know that we aren't bailing, and will continue to resolve seed hits.  

			} // while(!gw.done())
		} // for(size_t i = 0; i < gws_.size(); i++)
	} // while(neltLeft > 0 && eltsDone < maxelt)
	return false;
}

