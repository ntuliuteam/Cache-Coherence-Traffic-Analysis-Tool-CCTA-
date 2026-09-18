/*
 * Copyright (c) 2020-2021 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 1999-2012 Mark D. Hill and David A. Wood
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "mem/ruby/structures/CacheMemory.hh"

#include "base/compiler.hh"
#include "base/intmath.hh"
#include "base/logging.hh"
#include "debug/HtmMem.hh"
#include "debug/RubyCache.hh"
#include "debug/RubyCacheTrace.hh"
#include "debug/RubyResourceStalls.hh"
#include "debug/RubyStats.hh"
#include "mem/cache/replacement_policies/weighted_lru_rp.hh"
#include "mem/ruby/protocol/AccessPermission.hh"
#include "mem/ruby/system/RubySystem.hh"




namespace gem5
{

namespace ruby
{

std::ostream&
operator<<(std::ostream& out, const CacheMemory& obj)
{
    obj.print(out);
    out << std::flush;
    return out;
}

CacheMemory::CacheMemory(const Params &p)
    : SimObject(p),
    dataArray(p.dataArrayBanks, p.dataAccessLatency,
              p.start_index_bit, p.ruby_system),
    tagArray(p.tagArrayBanks, p.tagAccessLatency,
             p.start_index_bit, p.ruby_system),
    atomicALUArray(p.atomicALUs, p.atomicLatency *
             p.ruby_system->clockPeriod()),
    cacheMemoryStats(this)
{
    m_cache_size = p.size;
    m_cache_assoc = p.assoc;
    m_replacementPolicy_ptr = p.replacement_policy;
    m_start_index_bit = p.start_index_bit;
    m_is_instruction_only_cache = p.is_icache;
    m_resource_stalls = p.resourceStalls;
    m_block_size = p.block_size;  // may be 0 at this point. Updated in init()
    m_use_occupancy = dynamic_cast<replacement_policy::WeightedLRU*>(
                                    m_replacementPolicy_ptr) ? true : false;
}

void
CacheMemory::init()
{
    if (m_block_size == 0) {
        m_block_size = RubySystem::getBlockSizeBytes();
    }
    m_cache_num_sets = (m_cache_size / m_cache_assoc) / m_block_size;
    assert(m_cache_num_sets > 1);
    m_cache_num_set_bits = floorLog2(m_cache_num_sets);
    assert(m_cache_num_set_bits > 0);

    m_cache.resize(m_cache_num_sets,
                    std::vector<AbstractCacheEntry*>(m_cache_assoc, nullptr));
    replacement_data.resize(m_cache_num_sets,
                               std::vector<ReplData>(m_cache_assoc, nullptr));
    // instantiate all the replacement_data here
    for (int i = 0; i < m_cache_num_sets; i++) {
        for ( int j = 0; j < m_cache_assoc; j++) {
            replacement_data[i][j] =
                                m_replacementPolicy_ptr->instantiateEntry();
        }
    }
}

CacheMemory::~CacheMemory()
{
    if (m_replacementPolicy_ptr)
        delete m_replacementPolicy_ptr;
    for (int i = 0; i < m_cache_num_sets; i++) {
        for (int j = 0; j < m_cache_assoc; j++) {
            delete m_cache[i][j];
        }
    }
}

// convert a Address to its location in the cache
int64_t
CacheMemory::addressToCacheSet(Addr address) const
{
    assert(address == makeLineAddress(address));
    return bitSelect(address, m_start_index_bit,
                     m_start_index_bit + m_cache_num_set_bits - 1);
}

// Given a cache index: returns the index of the tag in a set.
// returns -1 if the tag is not found.
int
CacheMemory::findTagInSet(int64_t cacheSet, Addr tag) const
{
    assert(tag == makeLineAddress(tag));
    // search the set for the tags
    auto it = m_tag_index.find(tag);
    if (it != m_tag_index.end())
        if (m_cache[cacheSet][it->second]->m_Permission !=
            AccessPermission_NotPresent)
            return it->second;
    return -1; // Not found
}

// Given a cache index: returns the index of the tag in a set.
// returns -1 if the tag is not found.
int
CacheMemory::findTagInSetIgnorePermissions(int64_t cacheSet,
                                           Addr tag) const
{
    assert(tag == makeLineAddress(tag));
    // search the set for the tags
    auto it = m_tag_index.find(tag);
    if (it != m_tag_index.end())
        return it->second;
    return -1; // Not found
}

// Given an unique cache block identifier (idx): return the valid address
// stored by the cache block.  If the block is invalid/notpresent, the
// function returns the 0 address
Addr
CacheMemory::getAddressAtIdx(int idx) const
{
    Addr tmp(0);

    int set = idx / m_cache_assoc;
    assert(set < m_cache_num_sets);

    int way = idx - set * m_cache_assoc;
    assert (way < m_cache_assoc);

    AbstractCacheEntry* entry = m_cache[set][way];
    if (entry == NULL ||
        entry->m_Permission == AccessPermission_Invalid ||
        entry->m_Permission == AccessPermission_NotPresent) {
        return tmp;
    }
    return entry->m_Address;
}

bool
CacheMemory::tryCacheAccess(Addr address, RubyRequestType type,
                            DataBlock*& data_ptr)
{
    DPRINTF(RubyCache, "trying to access address: %#x\n", address);
    AbstractCacheEntry* entry = lookup(address);
    if (entry != nullptr) {
        // Do we even have a tag match?
        m_replacementPolicy_ptr->touch(entry->replacementData);
        entry->setLastAccess(curTick());
        data_ptr = &(entry->getDataBlk());

        if (entry->m_Permission == AccessPermission_Read_Write) {
            DPRINTF(RubyCache, "Have permission to access address: %#x\n",
                        address);
            return true;
        }
        if ((entry->m_Permission == AccessPermission_Read_Only) &&
            (type == RubyRequestType_LD || type == RubyRequestType_IFETCH)) {
            DPRINTF(RubyCache, "Have permission to access address: %#x\n",
                        address);
            return true;
        }
        // The line must not be accessible
    }
    DPRINTF(RubyCache, "Do not have permission to access address: %#x\n",
                address);
    data_ptr = NULL;
    return false;
}

bool
CacheMemory::testCacheAccess(Addr address, RubyRequestType type,
                             DataBlock*& data_ptr)
{
    DPRINTF(RubyCache, "testing address: %#x\n", address);
    AbstractCacheEntry* entry = lookup(address);
    if (entry != nullptr) {
        // Do we even have a tag match?
        m_replacementPolicy_ptr->touch(entry->replacementData);
        entry->setLastAccess(curTick());
        data_ptr = &(entry->getDataBlk());

        DPRINTF(RubyCache, "have permission for address %#x?: %d\n",
                    address,
                    entry->m_Permission != AccessPermission_NotPresent);
        return entry->m_Permission != AccessPermission_NotPresent;
    }

    DPRINTF(RubyCache, "do not have permission for address %#x\n",
                address);
    data_ptr = NULL;
    return false;
}

// tests to see if an address is present in the cache
bool
CacheMemory::isTagPresent(Addr address) const
{
    const AbstractCacheEntry* const entry = lookup(address);
    if (entry == nullptr) {
        // We didn't find the tag
        DPRINTF(RubyCache, "No tag match for address: %#x\n", address);
        return false;
    }
    DPRINTF(RubyCache, "address: %#x found\n", address);
    return true;
}

// Returns true if there is:
//   a) a tag match on this address or there is
//   b) an unused line in the same cache "way"
bool
CacheMemory::cacheAvail(Addr address) const
{
    assert(address == makeLineAddress(address));

    int64_t cacheSet = addressToCacheSet(address);

    for (int i = 0; i < m_cache_assoc; i++) {
        AbstractCacheEntry* entry = m_cache[cacheSet][i];
        if (entry != NULL) {
            if (entry->m_Address == address ||
                entry->m_Permission == AccessPermission_NotPresent) {
                // Already in the cache or we found an empty entry
                return true;
            }
        } else {
            return true;
        }
    }
    return false;
}

AbstractCacheEntry*
CacheMemory::allocate(Addr address, AbstractCacheEntry *entry)
{
    assert(address == makeLineAddress(address));
    assert(!isTagPresent(address));
    assert(cacheAvail(address));
    DPRINTF(RubyCache, "allocating address: %#x\n", address);

    // Find the first open slot
    int64_t cacheSet = addressToCacheSet(address);
    std::vector<AbstractCacheEntry*> &set = m_cache[cacheSet];
    for (int i = 0; i < m_cache_assoc; i++) {
        if (!set[i] || set[i]->m_Permission == AccessPermission_NotPresent) {
            if (set[i] && (set[i] != entry)) {
                warn_once("This protocol contains a cache entry handling bug: "
                    "Entries in the cache should never be NotPresent! If\n"
                    "this entry (%#x) is not tracked elsewhere, it will memory "
                    "leak here. Fix your protocol to eliminate these!",
                    address);
            }
            set[i] = entry;  // Init entry
            set[i]->m_Address = address;
            set[i]->m_Permission = AccessPermission_Invalid;
            DPRINTF(RubyCache, "Allocate clearing lock for addr: 0x%x\n",
                    address);
            set[i]->m_locked = -1;
            m_tag_index[address] = i;
            set[i]->setPosition(cacheSet, i);
            set[i]->replacementData = replacement_data[cacheSet][i];
            set[i]->setLastAccess(curTick());

            // Call reset function here to set initial value for different
            // replacement policies.
            m_replacementPolicy_ptr->reset(entry->replacementData);

            return entry;
        }
    }
    panic("Allocate didn't find an available entry");
}

void
CacheMemory::deallocate(Addr address)
{
    DPRINTF(RubyCache, "deallocating address: %#x\n", address);
    AbstractCacheEntry* entry = lookup(address);
    assert(entry != nullptr);
    m_replacementPolicy_ptr->invalidate(entry->replacementData);
    uint32_t cache_set = entry->getSet();
    uint32_t way = entry->getWay();
    delete entry;
    m_cache[cache_set][way] = NULL;
    m_tag_index.erase(address);
}

// Returns with the physical address of the conflicting cache line
Addr
CacheMemory::cacheProbe(Addr address) const
{
    assert(address == makeLineAddress(address));
    assert(!cacheAvail(address));

    int64_t cacheSet = addressToCacheSet(address);
    std::vector<ReplaceableEntry*> candidates;
    for (int i = 0; i < m_cache_assoc; i++) {
        candidates.push_back(static_cast<ReplaceableEntry*>(
                                                       m_cache[cacheSet][i]));
    }
    return m_cache[cacheSet][m_replacementPolicy_ptr->
                        getVictim(candidates)->getWay()]->m_Address;
}

// looks an address up in the cache
AbstractCacheEntry*
CacheMemory::lookup(Addr address)
{
    assert(address == makeLineAddress(address));
    int64_t cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    if (loc == -1) return NULL;
    return m_cache[cacheSet][loc];
}

// looks an address up in the cache
const AbstractCacheEntry*
CacheMemory::lookup(Addr address) const
{
    assert(address == makeLineAddress(address));
    int64_t cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    if (loc == -1) return NULL;
    return m_cache[cacheSet][loc];
}

// Sets the most recently used bit for a cache block
void
CacheMemory::setMRU(Addr address)
{
    AbstractCacheEntry* entry = lookup(makeLineAddress(address));
    if (entry != nullptr) {
        m_replacementPolicy_ptr->touch(entry->replacementData);
        entry->setLastAccess(curTick());
    }
}

void
CacheMemory::setMRU(AbstractCacheEntry *entry)
{
    assert(entry != nullptr);
    m_replacementPolicy_ptr->touch(entry->replacementData);
    entry->setLastAccess(curTick());
}

void
CacheMemory::setMRU(Addr address, int occupancy)
{
    AbstractCacheEntry* entry = lookup(makeLineAddress(address));
    if (entry != nullptr) {
        // m_use_occupancy can decide whether we are using WeightedLRU
        // replacement policy. Depending on different replacement policies,
        // use different touch() function.
        if (m_use_occupancy) {
            static_cast<replacement_policy::WeightedLRU*>(
                m_replacementPolicy_ptr)->touch(
                entry->replacementData, occupancy);
        } else {
            m_replacementPolicy_ptr->touch(entry->replacementData);
        }
        entry->setLastAccess(curTick());
    }
}

int
CacheMemory::getReplacementWeight(int64_t set, int64_t loc)
{
    assert(set < m_cache_num_sets);
    assert(loc < m_cache_assoc);
    int ret = 0;
    if (m_cache[set][loc] != NULL) {
        ret = m_cache[set][loc]->getNumValidBlocks();
        assert(ret >= 0);
    }

    return ret;
}

void
CacheMemory::recordCacheContents(int cntrl, CacheRecorder* tr) const
{
    uint64_t warmedUpBlocks = 0;
    [[maybe_unused]] uint64_t totalBlocks = (uint64_t)m_cache_num_sets *
                                         (uint64_t)m_cache_assoc;

    for (int i = 0; i < m_cache_num_sets; i++) {
        for (int j = 0; j < m_cache_assoc; j++) {
            if (m_cache[i][j] != NULL) {
                AccessPermission perm = m_cache[i][j]->m_Permission;
                RubyRequestType request_type = RubyRequestType_NULL;
                if (perm == AccessPermission_Read_Only) {
                    if (m_is_instruction_only_cache) {
                        request_type = RubyRequestType_IFETCH;
                    } else {
                        request_type = RubyRequestType_LD;
                    }
                } else if (perm == AccessPermission_Read_Write) {
                    request_type = RubyRequestType_ST;
                }

                if (request_type != RubyRequestType_NULL) {
                    Tick lastAccessTick;
                    lastAccessTick = m_cache[i][j]->getLastAccess();
                    tr->addRecord(cntrl, m_cache[i][j]->m_Address,
                                  0, request_type, lastAccessTick,
                                  m_cache[i][j]->getDataBlk());
                    warmedUpBlocks++;
                }
            }
        }
    }

    DPRINTF(RubyCacheTrace, "%s: %lli blocks of %lli total blocks"
            "recorded %.2f%% \n", name().c_str(), warmedUpBlocks,
            totalBlocks, (float(warmedUpBlocks) / float(totalBlocks)) * 100.0);
}

void
CacheMemory::print(std::ostream& out) const
{
    out << "Cache dump: " << name() << std::endl;
    for (int i = 0; i < m_cache_num_sets; i++) {
        for (int j = 0; j < m_cache_assoc; j++) {
            if (m_cache[i][j] != NULL) {
                out << "  Index: " << i
                    << " way: " << j
                    << " entry: " << *m_cache[i][j] << std::endl;
            } else {
                out << "  Index: " << i
                    << " way: " << j
                    << " entry: NULL" << std::endl;
            }
        }
    }
}

void
CacheMemory::printData(std::ostream& out) const
{
    out << "printData() not supported" << std::endl;
}

void
CacheMemory::setLocked(Addr address, int context)
{
    DPRINTF(RubyCache, "Setting Lock for addr: %#x to %d\n", address, context);
    AbstractCacheEntry* entry = lookup(address);
    assert(entry != nullptr);
    entry->setLocked(context);
}

void
CacheMemory::clearLocked(Addr address)
{
    DPRINTF(RubyCache, "Clear Lock for addr: %#x\n", address);
    AbstractCacheEntry* entry = lookup(address);
    assert(entry != nullptr);
    entry->clearLocked();
}

void
CacheMemory::clearLockedAll(int context)
{
    // iterate through every set and way to get a cache line
    for (auto i = m_cache.begin(); i != m_cache.end(); ++i) {
        std::vector<AbstractCacheEntry*> set = *i;
        for (auto j = set.begin(); j != set.end(); ++j) {
            AbstractCacheEntry *line = *j;
            if (line && line->isLocked(context)) {
                DPRINTF(RubyCache, "Clear Lock for addr: %#x\n",
                    line->m_Address);
                line->clearLocked();
            }
        }
    }
}

bool
CacheMemory::isLocked(Addr address, int context)
{
    AbstractCacheEntry* entry = lookup(address);
    assert(entry != nullptr);
    DPRINTF(RubyCache, "Testing Lock for addr: %#llx cur %d con %d\n",
            address, entry->m_locked, context);
    return entry->isLocked(context);
}

CacheMemory::
CacheMemoryStats::CacheMemoryStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(numDataArrayReads, "Number of data array reads"),
      ADD_STAT(numDataArrayWrites, "Number of data array writes"),
      ADD_STAT(numTagArrayReads, "Number of tag array reads"),
      ADD_STAT(numTagArrayWrites, "Number of tag array writes"),
      ADD_STAT(numTagArrayStalls, "Number of stalls caused by tag array"),
      ADD_STAT(numDataArrayStalls, "Number of stalls caused by data array"),
      ADD_STAT(numAtomicALUOperations, "Number of atomic ALU operations"),
      ADD_STAT(numAtomicALUArrayStalls, "Number of stalls caused by atomic ALU array"),
      ADD_STAT(htmTransCommitReadSet, "Read set size of a committed "
                                      "transaction"),
      ADD_STAT(htmTransCommitWriteSet, "Write set size of a committed "
                                       "transaction"),
      ADD_STAT(htmTransAbortReadSet, "Read set size of a aborted transaction"),
      ADD_STAT(htmTransAbortWriteSet, "Write set size of a aborted "
                                      "transaction"),
      ADD_STAT(m_demand_hits, "Number of cache demand hits"),
      ADD_STAT(m_demand_misses, "Number of cache demand misses"),
      ADD_STAT(m_demand_accesses, "Number of cache demand accesses",
               m_demand_hits + m_demand_misses),
      ADD_STAT(m_prefetch_hits, "Number of cache prefetch hits"),
      ADD_STAT(m_prefetch_misses, "Number of cache prefetch misses"),
      ADD_STAT(m_prefetch_accesses, "Number of cache prefetch accesses",
               m_prefetch_hits + m_prefetch_misses),


      //add this for collecting messages for cache coherence

      // MESI Three-Level: L0 <-> L1
      ADD_STAT(num_l0_l1_messages, "Number of cache coherence messages transfer from l0 to l1"),
      ADD_STAT(num_l1_l0_messages, "Number of cache coherence messages transfer from l1 to l0"),

      ADD_STAT(num_l1_l2_messages, "Number of cache coherence messages transfer from l1 to l2"),
      ADD_STAT(num_l1_l1_messages, "Number of cache coherence messages transfer from l1 to another l1"),
      ADD_STAT(num_l1_dir_messages, "Number of cache coherence messages transfer from l1 to directory controller"),
      ADD_STAT(num_dir_l1_messages, "Number of cache coherence messages transfer from directory controller to l1"),
      ADD_STAT(num_l2_l1_messages, "Number of cache coherence messages transfer from l2 to l1"),
      ADD_STAT(num_l2_dir_messages, "Number of cache coherence messages transfer from l2 to directory"),
      ADD_STAT(num_dir_l2_messages, "Number of cache coherence messages transfer from directory to l2"),
      ADD_STAT(num_l2_mem_messages, "Number of cache coherence messages transfer from l2 to memory controller when l2 write/read miss"),
      //ADD_STAT(num_dir_mem_messages, "Number of cache coherence messages transfer from directory to memory"),
      //ADD_STAT(num_l2readmiss_mem_dir_messages, "Number of cache coherence messages transfer from memory to directory controller when l2 read miss"),
      // Adding the total core-to-core messages statistic

      //ADD_STAT(num_mem_dir_messages, "Number of cache coherence messages transfer from memory to directory"),
      ADD_STAT(num_mem_l2_messages, "Number of cache coherence messages transfer from memory to l2 cache"),
      ADD_STAT(total_cache_level_messages, "Total cache-level communication",
               num_l0_l1_messages + num_l1_l0_messages +
               num_l1_l2_messages + num_l2_l1_messages +
               num_l1_l1_messages),
      ADD_STAT(total_cache_dir_messages, "Total communication between directory and cache",
               num_l1_dir_messages + num_l2_dir_messages + num_dir_l1_messages + num_dir_l2_messages ),
      ADD_STAT(total_l2cache_memory_messages, "Total l2cache-memory communication",
               num_l2_mem_messages + num_mem_l2_messages ),
      //ADD_STAT(total_cachecoherence_messages, "Total cache coherence messages",
               //num_l1_dir_messages + num_l1_l2_messages + num_l2_l1_messages + num_l2_dir_messages + num_l2_mem_messages + num_dir_mem_messages + num_mem_l2_messages + num_mem_dir_messages),
      
      //add this to collect the timer of CPU from the perspective of cache coherence, to check whether the CPU is delayed
      ADD_STAT(writeMissCounter, "Count the times when write miss occurs"),
      ADD_STAT(readMissCounter, "Count the times when read miss occurs"),
      ADD_STAT(writeHitCounter, "Count the times when write hit occurs"),
      ADD_STAT(totalWriteMissDuration, "Total duration when write miss occurs"),
      ADD_STAT(totalReadMissDuration, "Total duration when read miss occurs"),
      ADD_STAT(totalWriteHitDuration, "Total duration when write hit occurs"),
      ADD_STAT(averageWriteMissTime, "The average time when write miss",
               totalWriteMissDuration/writeMissCounter),
      ADD_STAT(averageReadMissTime, "The average time when read miss",
               totalReadMissDuration/readMissCounter),
      ADD_STAT(averageWriteHitTime, "The average time when write hit with S",
               totalWriteHitDuration/writeHitCounter),

      //add this for timestamp from the perspective of NoC to check whether CPU is delayed
      ADD_STAT(NoCwriteMissCounter, "Count the times when write miss occurs for NoC timestamp"),
      ADD_STAT(NoCreadMissCounter, "Count the times when read miss occurs for NoC timestamp"),
      ADD_STAT(NoCwriteHitCounter, "Count the times when write hit occurs for NoC timestamp"),
      ADD_STAT(totalNoCWriteMissDuration, "Total duration when write miss occurs in NOC"),
      ADD_STAT(totalNoCReadMissDuration, "Total duration when read miss occurs in NOC"),
      ADD_STAT(totalNoCWriteHitDuration, "Total duration when write hit occurs in NOC"),
      ADD_STAT(averageNoCWriteMissTime, "The average time when write miss in NoC",
               totalNoCWriteMissDuration / NoCwriteMissCounter),
      ADD_STAT(averageNOCReadMissTime, "The average time when read miss in NoC",
               totalNoCReadMissDuration / NoCreadMissCounter),
      ADD_STAT(averageNoCWriteHitTime, "The average time when write hit with S in NoC",
               totalNoCWriteHitDuration / NoCwriteHitCounter),
    
    




      //end add



      ADD_STAT(m_accessModeType, "")
{
    numDataArrayReads
        .flags(statistics::nozero);

    numDataArrayWrites
        .flags(statistics::nozero);

    numTagArrayReads
        .flags(statistics::nozero);

    numTagArrayWrites
        .flags(statistics::nozero);

    numTagArrayStalls
        .flags(statistics::nozero);

    numDataArrayStalls
        .flags(statistics::nozero);

    numAtomicALUOperations
        .flags(statistics::nozero);

    numAtomicALUArrayStalls
        .flags(statistics::nozero);

    htmTransCommitReadSet
        .init(8)
        .flags(statistics::pdf | statistics::dist | statistics::nozero |
            statistics::nonan);

    htmTransCommitWriteSet
        .init(8)
        .flags(statistics::pdf | statistics::dist | statistics::nozero |
            statistics::nonan);

    htmTransAbortReadSet
        .init(8)
        .flags(statistics::pdf | statistics::dist | statistics::nozero |
            statistics::nonan);

    htmTransAbortWriteSet
        .init(8)
        .flags(statistics::pdf | statistics::dist | statistics::nozero |
            statistics::nonan);

    m_prefetch_hits
        .flags(statistics::nozero);  //Added .flags(statistics::nozero) for all new statistics, ensuring they don't display zero values

    m_prefetch_misses
        .flags(statistics::nozero);

    m_prefetch_accesses
        .flags(statistics::nozero);

    m_accessModeType
        .init(RubyRequestType_NUM)
        .flags(statistics::pdf | statistics::total);

    for (int i = 0; i < RubyAccessMode_NUM; i++) {
        m_accessModeType
            .subname(i, RubyAccessMode_to_string(RubyAccessMode(i)))
            .flags(statistics::nozero)
            ;
    }
}

// assumption: SLICC generated files will only call this function
// once **all** resources are granted
void
CacheMemory::recordRequestType(CacheRequestType requestType, Addr addr)
{
    DPRINTF(RubyStats, "Recorded statistic: %s\n",
            CacheRequestType_to_string(requestType));
    switch(requestType) {
    case CacheRequestType_DataArrayRead:
        if (m_resource_stalls)
            dataArray.reserve(addressToCacheSet(addr));
        cacheMemoryStats.numDataArrayReads++;
        return;
    case CacheRequestType_DataArrayWrite:
        if (m_resource_stalls)
            dataArray.reserve(addressToCacheSet(addr));
        cacheMemoryStats.numDataArrayWrites++;
        return;
    case CacheRequestType_TagArrayRead:
        if (m_resource_stalls)
            tagArray.reserve(addressToCacheSet(addr));
        cacheMemoryStats.numTagArrayReads++;
        return;
    case CacheRequestType_TagArrayWrite:
        if (m_resource_stalls)
            tagArray.reserve(addressToCacheSet(addr));
        cacheMemoryStats.numTagArrayWrites++;
        return;
    case CacheRequestType_AtomicALUOperation:
        if (m_resource_stalls)
            atomicALUArray.reserve(addr);
        cacheMemoryStats.numAtomicALUOperations++;
        return;
    default:
        warn("CacheMemory access_type not found: %s",
             CacheRequestType_to_string(requestType));
    }
}

bool
CacheMemory::checkResourceAvailable(CacheResourceType res, Addr addr)
{
    if (!m_resource_stalls) {
        return true;
    }

    if (res == CacheResourceType_TagArray) {
        if (tagArray.tryAccess(addressToCacheSet(addr))) return true;
        else {
            DPRINTF(RubyResourceStalls,
                    "Tag array stall on addr %#x in set %d\n",
                    addr, addressToCacheSet(addr));
            cacheMemoryStats.numTagArrayStalls++;
            return false;
        }
    } else if (res == CacheResourceType_DataArray) {
        if (dataArray.tryAccess(addressToCacheSet(addr))) return true;
        else {
            DPRINTF(RubyResourceStalls,
                    "Data array stall on addr %#x in set %d\n",
                    addr, addressToCacheSet(addr));
            cacheMemoryStats.numDataArrayStalls++;
            return false;
        }
    } else if (res == CacheResourceType_AtomicALUArray) {
        if (atomicALUArray.tryAccess(addr)) return true;
        else {
            DPRINTF(RubyResourceStalls,
                    "Atomic ALU array stall on addr %#x in line address %#x\n",
                    addr, makeLineAddress(addr));
            cacheMemoryStats.numAtomicALUArrayStalls++;
            return false;
        }
    } else {
        panic("Unrecognized cache resource type.");
    }
}

bool
CacheMemory::isBlockInvalid(int64_t cache_set, int64_t loc)
{
  return (m_cache[cache_set][loc]->m_Permission == AccessPermission_Invalid);
}

bool
CacheMemory::isBlockNotBusy(int64_t cache_set, int64_t loc)
{
  return (m_cache[cache_set][loc]->m_Permission != AccessPermission_Busy);
}

/* hardware transactional memory */

void
CacheMemory::htmAbortTransaction()
{
    uint64_t htmReadSetSize = 0;
    uint64_t htmWriteSetSize = 0;

    // iterate through every set and way to get a cache line
    for (auto i = m_cache.begin(); i != m_cache.end(); ++i)
    {
        std::vector<AbstractCacheEntry*> set = *i;

        for (auto j = set.begin(); j != set.end(); ++j)
        {
            AbstractCacheEntry *line = *j;

            if (line != nullptr) {
                htmReadSetSize += (line->getInHtmReadSet() ? 1 : 0);
                htmWriteSetSize += (line->getInHtmWriteSet() ? 1 : 0);
                if (line->getInHtmWriteSet()) {
                    line->invalidateEntry();
                }
                line->setInHtmWriteSet(false);
                line->setInHtmReadSet(false);
                line->clearLocked();
            }
        }
    }

    cacheMemoryStats.htmTransAbortReadSet.sample(htmReadSetSize);
    cacheMemoryStats.htmTransAbortWriteSet.sample(htmWriteSetSize);
    DPRINTF(HtmMem, "htmAbortTransaction: read set=%u write set=%u\n",
        htmReadSetSize, htmWriteSetSize);
}

void
CacheMemory::htmCommitTransaction()
{
    uint64_t htmReadSetSize = 0;
    uint64_t htmWriteSetSize = 0;

    // iterate through every set and way to get a cache line
    for (auto i = m_cache.begin(); i != m_cache.end(); ++i)
    {
        std::vector<AbstractCacheEntry*> set = *i;

        for (auto j = set.begin(); j != set.end(); ++j)
        {
            AbstractCacheEntry *line = *j;
            if (line != nullptr) {
                htmReadSetSize += (line->getInHtmReadSet() ? 1 : 0);
                htmWriteSetSize += (line->getInHtmWriteSet() ? 1 : 0);
                line->setInHtmWriteSet(false);
                line->setInHtmReadSet(false);
                line->clearLocked();
             }
        }
    }

    cacheMemoryStats.htmTransCommitReadSet.sample(htmReadSetSize);
    cacheMemoryStats.htmTransCommitWriteSet.sample(htmWriteSetSize);
    DPRINTF(HtmMem, "htmCommitTransaction: read set=%u write set=%u\n",
        htmReadSetSize, htmWriteSetSize);
}


void
CacheMemory::profileDemandHit()
{
    cacheMemoryStats.m_demand_hits++;
}

void
CacheMemory::profileDemandMiss()
{
    cacheMemoryStats.m_demand_misses++;
}

void
CacheMemory::profilePrefetchHit()
{
    cacheMemoryStats.m_prefetch_hits++;
}

void
CacheMemory::profilePrefetchMiss()
{
    cacheMemoryStats.m_prefetch_misses++;
}



//add this for collecting messages for cache coherence

// MESI Three-Level: L0 -> L1
void
CacheMemory::profile_l0_l1()
{
    cacheMemoryStats.num_l0_l1_messages++;
}

// MESI Three-Level: L1 -> L0
void
CacheMemory::profile_l1_l0()
{
    cacheMemoryStats.num_l1_l0_messages++;
}

void
CacheMemory::profile_l1_l2()
{

    cacheMemoryStats.num_l1_l2_messages++;

}

void
CacheMemory::profile_l1_l1()
{

    cacheMemoryStats.num_l1_l1_messages++;

}
void
CacheMemory::profile_l1_dir()
{

    cacheMemoryStats.num_l1_dir_messages++;
}
void
CacheMemory::profile_dir_l1()
{

    cacheMemoryStats.num_dir_l1_messages++;
}

void
CacheMemory::profile_l2_l1()
{

    cacheMemoryStats.num_l2_l1_messages++;
}
void
CacheMemory::profile_l2_dir()
{

    cacheMemoryStats.num_l2_dir_messages++;
}
void
CacheMemory::profile_dir_l2()
{

    cacheMemoryStats.num_dir_l2_messages++;
}
void
CacheMemory::profile_l2_mem()
{

    cacheMemoryStats.num_l2_mem_messages++;
}
void
CacheMemory::profile_mem_l2()
{

    cacheMemoryStats.num_mem_l2_messages++;
}





//add function for timestamp from the perspective of cache coherence to check whether CPU is delayed or not
void 
CacheMemory::recordStartTime()
{
    startTime = curTick();
}
void 
CacheMemory::recordEndTime_writemiss() 
{
    double endTime = curTick();
    double duration = endTime - startTime;
    durations.push_back(duration);
    writeMissCounter++;
    totalWriteMissDuration += duration;
    
    cacheMemoryStats.writeMissCounter = writeMissCounter;
    cacheMemoryStats.totalWriteMissDuration = totalWriteMissDuration;

    
}
void 
CacheMemory::recordEndTime_readmiss() 
{
    double endTime = curTick();
    double duration = endTime - startTime;
    durations.push_back(duration);
    readMissCounter++;
    totalReadMissDuration += duration;

    cacheMemoryStats.readMissCounter= readMissCounter;
    cacheMemoryStats.totalReadMissDuration = totalReadMissDuration;
    
}
void 
CacheMemory::recordEndTime_writehit() 
{
    double endTime = curTick();
    double duration = endTime - startTime;
    durations.push_back(duration);
    writeHitCounter++;
    totalWriteHitDuration += duration;

    cacheMemoryStats.writeHitCounter= writeHitCounter;
    cacheMemoryStats.totalWriteHitDuration = totalWriteHitDuration;    
}


//add this for the timestamp from the perspective of NoC to check whether CPU is delayed or not 
void 
CacheMemory::logNoCStartTime() {
    NoCstartTime = curTick();
}

void 
CacheMemory::logNoCEndTime_writemiss() {
    double endTime = curTick();
    double duration = endTime - NoCstartTime;
    NoCdurations.push_back(duration);

    NoCwriteMissCounter++;
    totalNoCWriteMissDuration += duration;
    
    cacheMemoryStats.NoCwriteMissCounter = NoCwriteMissCounter;
    cacheMemoryStats.totalNoCWriteMissDuration = totalNoCWriteMissDuration;
}

void
CacheMemory::logNoCEndTime_readmiss() {
    double endTime = curTick();
    double duration = endTime - NoCstartTime;
    NoCdurations.push_back(duration);
    NoCreadMissCounter++;
    totalNoCReadMissDuration += duration;

    cacheMemoryStats.NoCreadMissCounter= NoCreadMissCounter;
    cacheMemoryStats.totalNoCReadMissDuration = totalNoCReadMissDuration;
}

void
CacheMemory::logNoCEndTime_writehit() {
    double endTime = curTick();
    double duration = endTime - NoCstartTime;
    NoCdurations.push_back(duration);
    NoCwriteHitCounter++;
    totalNoCWriteHitDuration += duration;

    cacheMemoryStats.NoCwriteHitCounter= NoCwriteHitCounter;
    cacheMemoryStats.totalNoCWriteHitDuration = totalNoCWriteHitDuration;
}

//bool
//CacheMemory::IsWriteHitFlag()
//{
    //m_is_write_hit = true;
    // Create or overwrite a file to indicate a write hit has occurred
    //std::ofstream outfile("/data/guochu/gem5/16c_routing/write_hit_flag.txt");
    //if (outfile.is_open()) {
        //outfile << "Write hit detected." << std::endl;
        //outfile.close();
    //} else {
        //warn("Unable to create write hit flag file.");
    //}

    //return m_is_write_hit;
//}



//int 
//CacheMemory::generateUniqueEventId() {
    //return ++currentEventId;
////}

//void 
//CacheMemory::printNoCDurations(){
    //for (const auto& entry : eventDurations) {
        //std:cout << "Event ID: " << entry.first << " Duration: " << entry.second.count() << " seconds" << std::endl;
        //DPRINTF(RubyNetwork, "Event ID: %d Duration: %f seconds\n", entry.first, entry.second.count());
        
    //}
//}

//void
//CacheMemory::logNoCDuration_writemiss(double duration){
    //totalNoCWriteMissDuration + = duration;
    //cacheMemoryStats.totalNoCWriteMissDuration = totalNoCWriteMissDuration;
//}

//void
//CacheMemory::logNoCDuration_readmiss(double duration){
    //totalNoCReadMissDuration + = duration;
    //cacheMemoryStats.totalNoCReadMissDuration = totalNoCReadMissDuration;
//}

//void
//CacheMemory::logNoCDuration_writehit(double duration){
    //totalNoCWritehitDuration + = duration;
    //cacheMemoryStats.totalNoCWriteHitDuration = totalNoCWriteHitDuration;
//}


//functions to determine the event triggered in L1 cache
//void
//CacheMemory::judge_L1_event(Addr address, RubyRequestType requestType, int eventId) {
    //AbstractCacheEntry* entry = lookup(address);

    //AccessPermission perm = entry->m_Permission;

    //std::string eventType;
    //if (entry == NULL || perm == AccessPermission_Invalid || perm == AccessPermission_NotPresent) {
        //if (requestType == RubyRequestType_ST) {
            //eventType = "writemiss";
        //}
        //else if (requestType == RubyRequestType_LD) {
            //eventType = "readmiss";

        //}
    //}else if (perm == AccessPermission_Read_Only && requestType == RubyRequestType_ST) {

            //eventType = "writehitS";
    //}
    
        //return eventType;
        //int eventId = generateUniqueEventId();
        //logNoCStartTime(eventId, eventType);
        //eventTypes[eventId] = eventType;
    
//}

//std::string
//CacheMemory::getEventType(int eventId) {
    //auto it = eventTypes.find(eventId);
    //if (it != eventTypes.end()) {
        //return eventTypes[eventId];
    //} else {
        //panic("Event ID not found in eventTypes map");
    //}
//}
//end add



} // namespace ruby
} // namespace gem5
