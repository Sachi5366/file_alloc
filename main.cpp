#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace std;

constexpr size_t BLOCK_SIZE = 512u;
constexpr size_t FILE_SIZE_BYTES = 1024u * 1024u;               // 1 MB
constexpr size_t REQ_BLOCKS = FILE_SIZE_BYTES / BLOCK_SIZE;     // 2048
constexpr size_t TOTAL_BLOCKS = 10000u;                         // example disk size
constexpr size_t MAX_PRINT = 16u;

enum class BlockState : uint8_t { FREE = 0, USED = 1 };

struct Disk {
    vector<BlockState> blocks;
    Disk(size_t total = TOTAL_BLOCKS) : blocks(total, BlockState::FREE) {}
    size_t total_blocks() const { return blocks.size(); }
    size_t count_free() const {
        size_t c = 0;
        for (auto &b : blocks) if (b == BlockState::FREE) ++c;
        return c;
    }
    size_t largest_free_run() const {
        size_t best = 0, cur = 0;
        for (auto &b : blocks) {
            if (b == BlockState::FREE) ++cur; else cur = 0;
            if (cur > best) best = cur;
        }
        return best;
    }
};

void print_block_list(const vector<size_t>& list) {
    if (list.empty()) {
        cout << "  (none)\n";
        return;
    }
    cout << "  [";
    size_t to = min(list.size(), MAX_PRINT);
    for (size_t i = 0; i < to; ++i) {
        if (i) cout << ", ";
        cout << list[i];
    }
    if (list.size() > MAX_PRINT) cout << ", ... (total " << list.size() << " blocks)";
    cout << "]\n";
}

// Clone disk
Disk clone_disk(const Disk& d) {
    Disk out(d.total_blocks());
    out.blocks = d.blocks;
    return out;
}

/* Sequential (contiguous) allocation:
   returns pair(success, probe_count), and writes start index if success.
*/
pair<bool, long long> allocate_sequential(Disk &d, size_t req, ssize_t &start_out) {
    size_t n = d.total_blocks();
    long long probes = 0;
    for (size_t i = 0; i + req <= n; ++i) {
        bool ok = true;
        for (size_t j = 0; j < req; ++j) {
            ++probes;
            if (d.blocks[i + j] != BlockState::FREE) { ok = false; break; }
        }
        if (ok) {
            for (size_t j = 0; j < req; ++j) d.blocks[i + j] = BlockState::USED;
            start_out = static_cast<ssize_t>(i);
            return {true, probes};
        }
    }
    start_out = -1;
    return {false, probes};
}

/* Indexed allocation:
   find an index block then req data blocks anywhere. returns success and probe count.
   index_list will contain the data block indices if success.
*/
pair<bool, long long> allocate_indexed(Disk &d, size_t req, size_t &index_block_out, vector<size_t> &index_list) {
    size_t n = d.total_blocks();
    long long probes = 0;
    index_list.clear();
    index_block_out = static_cast<size_t>(-1);

    // find index block
    for (size_t i = 0; i < n; ++i) {
        ++probes;
        if (d.blocks[i] == BlockState::FREE) {
            d.blocks[i] = BlockState::USED;
            index_block_out = i;
            break;
        }
    }
    if (index_block_out == static_cast<size_t>(-1)) return {false, probes};

    // find req data blocks
    size_t allocated = 0;
    for (size_t i = 0; i < n && allocated < req; ++i) {
        ++probes;
        if (d.blocks[i] == BlockState::FREE) {
            d.blocks[i] = BlockState::USED;
            index_list.push_back(i);
            ++allocated;
        }
    }
    if (allocated < req) {
        // rollback
        d.blocks[index_block_out] = BlockState::FREE;
        for (auto idx : index_list) d.blocks[idx] = BlockState::FREE;
        index_list.clear();
        index_block_out = static_cast<size_t>(-1);
        return {false, probes};
    }
    return {true, probes};
}

/* Linked allocation:
   pick req free blocks anywhere. link_list_out receives sequence of block indices.
*/
pair<bool, long long> allocate_linked(Disk &d, size_t req, vector<size_t> &link_list_out) {
    size_t n = d.total_blocks();
    long long probes = 0;
    link_list_out.clear();
    for (size_t i = 0; i < n && link_list_out.size() < req; ++i) {
        ++probes;
        if (d.blocks[i] == BlockState::FREE) {
            d.blocks[i] = BlockState::USED;
            link_list_out.push_back(i);
        }
    }
    if (link_list_out.size() < req) {
        // rollback
        for (auto idx : link_list_out) d.blocks[idx] = BlockState::FREE;
        link_list_out.clear();
        return {false, probes};
    }
    return {true, probes};
}

/* Fragment the disk:
   Leave at least min_free free blocks, then break large runs >= min_run_to_break
*/
void fragment_disk(Disk &d, size_t min_free, size_t min_run_to_break, std::mt19937 &rng) {
    size_t total = d.total_blocks();
    // mark random blocks used until free_count == min_free
    size_t free_count = total;
    uniform_int_distribution<size_t> dist(0, total - 1);
    while (free_count > min_free) {
        size_t idx = dist(rng);
        if (d.blocks[idx] == BlockState::FREE) {
            d.blocks[idx] = BlockState::USED;
            --free_count;
        }
    }
    // break large runs
    int attempts = 0;
    while (d.largest_free_run() >= min_run_to_break && attempts < static_cast<int>(total) * 5) {
        bool found = false;
        for (size_t i = 0; i < total; ++i) {
            if (d.blocks[i] == BlockState::FREE) {
                size_t j = i;
                while (j < total && d.blocks[j] == BlockState::FREE) ++j;
                size_t runlen = j - i;
                if (runlen >= min_run_to_break) {
                    size_t pos = i + (dist(rng) % runlen);
                    d.blocks[pos] = BlockState::USED;
                    found = true;
                    break;
                }
                i = j;
            }
        }
        if (!found) break;
        ++attempts;
    }
}

void run_allocations(const Disk &base, const string &scenario_name) {
    cout << "\n=== Scenario: " << scenario_name << " ===\n";
    cout << "Disk total blocks = " << base.total_blocks()
         << ", free blocks = " << base.count_free()
         << ", largest contiguous free run = " << base.largest_free_run() << "\n";
    cout << "File requires " << REQ_BLOCKS << " blocks (file size " << FILE_SIZE_BYTES
         << " bytes, block size " << BLOCK_SIZE << " bytes)\n";

    // Sequential
    Disk d1 = clone_disk(base);
    ssize_t start = -1;
    auto seq_res = allocate_sequential(d1, REQ_BLOCKS, start);
    if (seq_res.first) {
        cout << "\nSequential (contiguous) allocation: SUCCESS\n";
        cout << "  Start block = " << start << ", blocks allocated = " << REQ_BLOCKS << "\n";
        cout << "  Probe count (search checks) = " << seq_res.second << "\n";
    } else {
        cout << "\nSequential (contiguous) allocation: FAILED (no contiguous run)\n";
        cout << "  Probe count (search checks performed) = " << seq_res.second << "\n";
    }
    cout << "  After attempt: free blocks = " << d1.count_free()
         << ", largest free run = " << d1.largest_free_run() << "\n";

    // Indexed
    Disk d2 = clone_disk(base);
    vector<size_t> index_list;
    size_t index_block = static_cast<size_t>(-1);
    auto idx_res = allocate_indexed(d2, REQ_BLOCKS, index_block, index_list);
    if (idx_res.first) {
        cout << "\nIndexed allocation: SUCCESS\n";
        cout << "  Index block at = " << index_block << " (overhead 1 block)\n";
        cout << "  Data blocks (first few):\n";
        print_block_list(index_list);
        cout << "  Probe count (search checks) = " << idx_res.second << "\n";
    } else {
        cout << "\nIndexed allocation: FAILED (not enough free blocks)\n";
        cout << "  Probe count (search checks) = " << idx_res.second << "\n";
    }
    cout << "  After attempt: free blocks = " << d2.count_free()
         << ", largest free run = " << d2.largest_free_run() << "\n";

    // Linked
    Disk d3 = clone_disk(base);
    vector<size_t> link_list;
    auto link_res = allocate_linked(d3, REQ_BLOCKS, link_list);
    if (link_res.first) {
        cout << "\nLinked allocation: SUCCESS\n";
        cout << "  First few block indices in linked chain:\n";
        print_block_list(link_list);
        cout << "  Probe count (search checks) = " << link_res.second << "\n";
    } else {
        cout << "\nLinked allocation: FAILED (not enough free blocks)\n";
        cout << "  Probe count (search checks) = " << link_res.second << "\n";
    }
    cout << "  After attempt: free blocks = " << d3.count_free()
         << ", largest free run = " << d3.largest_free_run() << "\n";

    cout << "\nEfficiency summary (qualitative):\n";
    cout << " - Sequential: best read performance (contiguous), low metadata overhead, fails under fragmentation.\n";
    cout << " - Indexed: always works if enough free blocks; index overhead = 1 block; random access fine but scattered blocks increase seeks.\n";
    cout << " - Linked: always works if enough free blocks; no index block but pointer storage required and random reads are costly.\n";
}

int main() {
    std::mt19937 rng(static_cast<unsigned>(
        chrono::system_clock::now().time_since_epoch().count()));

    // base disk (all free)
    Disk base(TOTAL_BLOCKS);

    // Scenario A: contiguous-available
    Disk contig = clone_disk(base);
    // mark some blocks used at start and end, leave a big free run in middle
    for (size_t i = 0; i < 1500 && i < contig.total_blocks(); ++i) contig.blocks[i] = BlockState::USED;
    size_t end_start = 1500 + REQ_BLOCKS + 100;
    for (size_t i = end_start; i < contig.total_blocks(); ++i) contig.blocks[i] = BlockState::USED;

    run_allocations(contig, "Contiguous-available (large free run present)");

    // Scenario B: fragmented
    Disk frag = clone_disk(base);
    // Leave at least REQ_BLOCKS free but fragment the disk
    size_t min_free = REQ_BLOCKS;
    size_t min_run_to_break = REQ_BLOCKS;
    fragment_disk(frag, min_free, min_run_to_break, rng);

    if (frag.count_free() < REQ_BLOCKS) {
        cerr << "Fragmentation procedure left too few free blocks (" << frag.count_free() << ") — aborting fragmented scenario\n";
    } else {
        run_allocations(frag, "Fragmented (no large contiguous run; only non-contiguous free blocks)");
    }

    return 0;
}
