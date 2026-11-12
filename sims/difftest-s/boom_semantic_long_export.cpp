#include "boom_semantic_long_export.h"
#include "rvwmo/producer_ids.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <openssl/sha.h>
#include <set>
#include <sqlite3.h>
#include <sys/stat.h>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace difftest_s::native {
namespace {
constexpr std::string_view kClock = "boom_tile_clock_posedge_pre_nba";
constexpr size_t kMaxChunkBytes = 1024 * 1024;
constexpr size_t kMaxChunkRows = 4096;
constexpr uint64_t kLineBytes = 64;

std::string json_string(std::string_view value) {
  std::ostringstream output;
  output << '"';
  constexpr char hex[] = "0123456789abcdef";
  for (const unsigned char c : value) {
    if (c == '"' || c == '\\') output << '\\' << static_cast<char>(c);
    else if (c < 0x20) output << "\\u00" << hex[c >> 4] << hex[c & 15];
    else output << static_cast<char>(c);
  }
  output << '"';
  return output.str();
}

std::string sha256(const std::string &data) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  if (!SHA256(reinterpret_cast<const unsigned char *>(data.data()), data.size(), digest))
    throw std::runtime_error("BOOM semantic chunk SHA-256 failed");
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(2 * SHA256_DIGEST_LENGTH);
  for (auto c : digest) { result += hex[c >> 4]; result += hex[c & 15]; }
  return result;
}

void write_exclusive(const std::string &path, const std::string &data) {
  const auto partial = path + ".partial." + std::to_string(::getpid());
  const int fd = ::open(partial.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (fd < 0) throw std::runtime_error("cannot create BOOM semantic temporary output: " + partial);
  bool open = true;
  try {
    for (size_t offset = 0; offset < data.size();) {
      const ssize_t written = ::write(fd, data.data() + offset, data.size() - offset);
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0) throw std::runtime_error("failed writing BOOM semantic chunk");
      offset += static_cast<size_t>(written);
    }
    const int sync_result = ::fsync(fd);
    const int close_result = ::close(fd);
    open = false;
    if (sync_result || close_result)
      throw std::runtime_error("failed syncing BOOM semantic chunk");
    if (::link(partial.c_str(), path.c_str()))
      throw std::runtime_error("BOOM semantic chunk already exists or cannot be published");
    ::unlink(partial.c_str());
  } catch (...) {
    if (open) ::close(fd);
    ::unlink(partial.c_str());
    throw;
  }
}

uint32_t packed_mask(const std::vector<uint8_t> &lanes) {
  if (lanes.size() > 8) throw std::invalid_argument("BOOM semantic mask exceeds one scalar word");
  uint32_t mask = 0;
  for (size_t i = 0; i < lanes.size(); ++i)
    if (lanes[i]) mask |= uint32_t{1} << i;
  return mask;
}

uint32_t checked_mask(uint32_t mask, uint64_t address, uint32_t lanes) {
  if (!mask || !lanes || lanes > 8 || (mask & ~((uint32_t{1} << lanes) - 1)))
    throw std::runtime_error("BOOM semantic final byte mask is empty or exceeds one scalar word");
  for (uint32_t lane = 0; lane < lanes; ++lane)
    if ((mask & (uint32_t{1} << lane)) && address % kLineBytes + lane >= kLineBytes)
      throw std::runtime_error("BOOM semantic final byte mask crosses its physical line");
  return mask;
}

std::string producer_name(uint32_t id) {
  for (uint32_t hart = 0; hart < 4; ++hart) {
    if (id == rvwmo::hart_producer_id(hart)) return "h" + std::to_string(hart);
    if (id == rvwmo::memory_fabric_producer_id(hart)) return "f" + std::to_string(hart);
  }
  throw std::runtime_error("BOOM semantic observation has an unregistered producer");
}

void sql_exec(sqlite3 *db, const char *sql) {
  char *error = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string detail = error ? error : sqlite3_errmsg(db);
    sqlite3_free(error);
    throw std::runtime_error("BOOM semantic disk index: " + detail);
  }
}

class Statement {
public:
  Statement(sqlite3 *db, const char *sql) : db_(db) {
    if (sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK)
      throw std::runtime_error(std::string("BOOM semantic SQL prepare: ") + sqlite3_errmsg(db));
  }
  ~Statement() { sqlite3_finalize(statement_); }
  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;
  void bind(int index, uint64_t value) {
    if (sqlite3_bind_int64(statement_, index, static_cast<sqlite3_int64>(value)) != SQLITE_OK)
      throw std::runtime_error("BOOM semantic SQL bind failed");
  }
  bool next() {
    const int status = sqlite3_step(statement_);
    if (status == SQLITE_ROW) return true;
    if (status == SQLITE_DONE) return false;
    throw std::runtime_error(std::string("BOOM semantic disk index: ") + sqlite3_errmsg(db_));
  }
  void done() {
    if (next()) throw std::runtime_error("BOOM semantic SQL insert returned a row");
    reset();
  }
  void reset() {
    sqlite3_reset(statement_);
    sqlite3_clear_bindings(statement_);
  }
  uint64_t number(int index) const {
    return static_cast<uint64_t>(sqlite3_column_int64(statement_, index));
  }
private:
  sqlite3 *db_;
  sqlite3_stmt *statement_ = nullptr;
};

void bind_id(Statement &statement, const rvwmo::OperationId &id, int start = 1) {
  statement.bind(start, id.hart);
  statement.bind(start + 1, id.epoch);
  statement.bind(start + 2, id.uid);
  statement.bind(start + 3, id.subaccess);
}

std::string chunk_name(uint64_t window, uint64_t part) {
  char name[96];
  std::snprintf(name, sizeof name, "interaction-feedback.%08llu.%04llu.json",
      static_cast<unsigned long long>(window), static_cast<unsigned long long>(part));
  return name;
}

} // namespace

struct BoomSemanticLongExport::Impl {
  std::string output, case_id, run_id, elf, backend, layout, database_path;
  uint64_t first_cycle, max_cycle_budget, chunk_cycles, max_pair_gap_cycles;
  sqlite3 *db = nullptr;
  bool published = false;
  std::vector<std::string> published_chunks;

  Impl(std::string output_path, std::string case_name, std::string run_name,
      std::string elf_hash, std::string backend_hash, std::string layout_hash,
      uint64_t first, uint64_t horizon, uint64_t width, uint64_t gap)
      : output(std::move(output_path)), case_id(std::move(case_name)), run_id(std::move(run_name)),
        elf(std::move(elf_hash)), backend(std::move(backend_hash)), layout(std::move(layout_hash)),
        first_cycle(first), max_cycle_budget(horizon), chunk_cycles(width), max_pair_gap_cycles(gap) {
    if (first != 0 || !width || width > 1048576 || !gap || gap > 1048576 || first > horizon ||
        horizon >= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
      throw std::invalid_argument("BOOM semantic layout range, chunk width or pair gap is invalid");
    // The provisional join index is not a published artifact. Place it on local
    // scratch to avoid one NFS round trip per indexed event in long runs.
    const char *scratch_env = std::getenv("DIFFTEST_S_SEMANTIC_INDEX_DIR");
    const auto scratch = scratch_env && *scratch_env
        ? std::filesystem::path(scratch_env) : std::filesystem::temp_directory_path();
    if (!scratch.is_absolute() || !std::filesystem::is_directory(scratch))
      throw std::invalid_argument("BOOM semantic index scratch must be an absolute directory");
    // mkstemp adds a per-case random suffix; scratch remains private (0600).
    const auto pattern = (scratch / ("boom-semantic-" + sha256(output).substr(0, 16) +
        ".index.partial." + std::to_string(::getpid()) + ".XXXXXX")).string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back(0);
    const int fd = ::mkstemp(name.data());
    if (fd < 0) throw std::runtime_error("cannot create private BOOM semantic disk index");
    database_path = name.data();
    const int permission_result = ::fchmod(fd, 0600);
    const int close_result = ::close(fd);
    if (permission_result || close_result) {
      std::filesystem::remove(database_path);
      throw std::runtime_error("cannot secure BOOM semantic disk index");
    }
    try {
      if (sqlite3_open_v2(database_path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
        throw std::runtime_error("cannot open BOOM semantic disk index");
      sql_exec(db, "PRAGMA journal_mode=MEMORY; PRAGMA synchronous=OFF;"
          "PRAGMA cache_size=-16384; PRAGMA temp_store=FILE;"
          "CREATE TABLE retire_ids (hart INTEGER,epoch INTEGER,uid INTEGER,PRIMARY KEY(hart,epoch,uid));"
          "CREATE TABLE retired (hart INTEGER,epoch INTEGER,uid INTEGER,subaccess INTEGER,pc INTEGER,"
          "attempt INTEGER,address INTEGER,mask INTEGER,lanes INTEGER,flags INTEGER,"
          "PRIMARY KEY(hart,epoch,uid,subaccess));"
          "CREATE TABLE canceled (hart INTEGER,epoch INTEGER,uid INTEGER,subaccess INTEGER,attempt INTEGER,"
          "PRIMARY KEY(hart,epoch,uid,subaccess,attempt));"
          "CREATE TABLE squashed (hart INTEGER,epoch INTEGER,uid INTEGER,PRIMARY KEY(hart,epoch,uid));"
          "CREATE TABLE samples (id INTEGER PRIMARY KEY,window INTEGER,producer INTEGER,seq INTEGER,cycle INTEGER,"
          "phase INTEGER,kind INTEGER,hart INTEGER,epoch INTEGER,uid INTEGER,subaccess INTEGER,attempt INTEGER,"
          "address INTEGER,mask INTEGER,lanes INTEGER,tx_id INTEGER,generation INTEGER,"
          "byte_count INTEGER,coalesced INTEGER);"
          "CREATE TABLE members (sample_id INTEGER,ordinal INTEGER,hart INTEGER,epoch INTEGER,uid INTEGER,"
          "subaccess INTEGER,attempt INTEGER,generation INTEGER,PRIMARY KEY(sample_id,ordinal));"
          "CREATE INDEX samples_by_window ON samples(window,cycle,phase,producer,seq);"
          "CREATE TABLE emitted_loads (hart INTEGER,epoch INTEGER,uid INTEGER,subaccess INTEGER,attempt INTEGER,"
          "PRIMARY KEY(hart,epoch,uid,subaccess,attempt));"
          "CREATE TABLE observed_sources (producer INTEGER,seq INTEGER,PRIMARY KEY(producer,seq));"
          "CREATE TABLE emitted_sources (producer INTEGER,seq INTEGER,PRIMARY KEY(producer,seq));"
          "CREATE TABLE emitted_transactions (producer INTEGER,tx_id INTEGER,generation INTEGER,"
          "PRIMARY KEY(producer,tx_id,generation));"
          "BEGIN IMMEDIATE;");
    } catch (...) {
      if (db) sqlite3_close(db);
      db = nullptr;
      std::filesystem::remove(database_path);
      throw;
    }
  }
  ~Impl() {
    if (db) sqlite3_close(db);
    std::error_code ignored;
    std::filesystem::remove(database_path, ignored);
    std::filesystem::remove(database_path + "-journal", ignored);
    if (!published)
      for (const auto &path : published_chunks) std::filesystem::remove(path, ignored);
  }

  void observe(const rvwmo::RuntimeEvent &event) {
    using rvwmo::RuntimeEventKind;
    if (published) throw std::runtime_error("BOOM semantic feedback already published");
    if (event.kind == RuntimeEventKind::InstructionRetire) {
      const auto &retired = std::get<rvwmo::InstructionRetireEvent>(event.payload).instruction;
      Statement instruction(db, "INSERT INTO retire_ids VALUES(?,?,?)");
      instruction.bind(1, retired.hart); instruction.bind(2, retired.epoch);
      instruction.bind(3, retired.uid); instruction.done();
      Statement memory(db, "INSERT INTO retired VALUES(?,?,?,?,?,?,?,?,?,?)");
      for (const auto &operation : retired.memory) {
        bind_id(memory, operation.id);
        memory.bind(5, retired.pc); memory.bind(6, operation.attempt);
        memory.bind(7, operation.address); memory.bind(8, packed_mask(operation.byte_mask));
        memory.bind(9, operation.byte_mask.size());
        const uint32_t flags = static_cast<uint32_t>(operation.load) |
            (static_cast<uint32_t>(operation.store) << 1) |
            (static_cast<uint32_t>(operation.atomic) << 2) |
            (static_cast<uint32_t>(operation.lr) << 3) |
            (static_cast<uint32_t>(operation.sc) << 4);
        memory.bind(10, flags); memory.done();
      }
      return;
    }
    if (event.kind == RuntimeEventKind::InstructionCancelAttempt) {
      const auto &cancel = std::get<rvwmo::InstructionCancelAttemptEvent>(event.payload);
      Statement insert(db, "INSERT OR IGNORE INTO canceled VALUES(?,?,?,?,?)");
      bind_id(insert, cancel.id); insert.bind(5, cancel.attempt); insert.done();
      return;
    }
    if (event.kind == RuntimeEventKind::InstructionSquash) {
      const auto &id = std::get<rvwmo::InstructionSquashEvent>(event.payload).id;
      Statement insert(db, "INSERT OR IGNORE INTO squashed VALUES(?,?,?)");
      insert.bind(1, id.hart); insert.bind(2, id.epoch); insert.bind(3, id.uid); insert.done();
      return;
    }
    if (event.kind != RuntimeEventKind::LoadSample && event.kind != RuntimeEventKind::StorePublish)
      return;
    if (event.source.cycle < first_cycle || event.source.cycle > max_cycle_budget)
      throw std::runtime_error("BOOM semantic memory event falls outside the pinned scoring horizon");
    if (!event.source.producer_sequence || !event.source.order_qualified ||
        event.source.qualified_phase > 1)
      throw std::runtime_error("BOOM semantic memory source position is unqualified");
    producer_name(event.producer_id);
    Statement source(db, "INSERT INTO observed_sources VALUES(?,?)");
    source.bind(1, event.producer_id); source.bind(2, event.source.producer_sequence); source.done();
    Statement sample(db, "INSERT INTO samples(window,producer,seq,cycle,phase,kind,hart,epoch,uid,subaccess,"
        "attempt,address,mask,lanes,tx_id,generation,byte_count,coalesced)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    sample.bind(1, (event.source.cycle - first_cycle) / chunk_cycles);
    sample.bind(2, event.producer_id); sample.bind(3, event.source.producer_sequence);
    sample.bind(4, event.source.cycle); sample.bind(5, event.source.qualified_phase);
    const bool load = event.kind == RuntimeEventKind::LoadSample;
    sample.bind(6, load ? 1 : 2);
    if (load) {
      const auto &operation = std::get<rvwmo::LoadSampleEvent>(event.payload).operation;
      bind_id(sample, operation.id, 7); sample.bind(11, operation.attempt);
      sample.bind(12, operation.address); sample.bind(13, packed_mask(operation.byte_mask));
      sample.bind(14, operation.byte_mask.size());
      for (int column = 15; column <= 18; ++column) sample.bind(column, 0);
      sample.done();
      return;
    }
    const auto &store = std::get<rvwmo::StorePublishEvent>(event.payload);
    const auto id = store.members.empty() ? rvwmo::OperationId{} : store.members.front().id;
    bind_id(sample, id, 7);
    sample.bind(11, 0); sample.bind(12, store.address);
    sample.bind(13, packed_mask(store.byte_mask)); sample.bind(14, store.byte_mask.size());
    sample.bind(15, store.transaction_id); sample.bind(16, store.allocation_generation);
    sample.bind(17, store.bytes.size()); sample.bind(18, store.coalesced_program_order);
    sample.done();
    const uint64_t row_id = static_cast<uint64_t>(sqlite3_last_insert_rowid(db));
    Statement member(db, "INSERT INTO members VALUES(?,?,?,?,?,?,?,?)");
    for (size_t index = 0; index < store.members.size(); ++index) {
      const auto &entry = store.members[index];
      member.bind(1, row_id); member.bind(2, index);
      bind_id(member, entry.id, 3);
      member.bind(7, entry.attempt); member.bind(8, entry.memory_generation);
      member.done();
    }
  }

  void publish(const std::map<uint32_t, rvwmo::SourcePosition> &watermarks, uint64_t completed_cycle) {
    if (!completed_cycle || completed_cycle > max_cycle_budget)
      throw std::runtime_error("BOOM semantic checked completion exceeds the pinned cycle budget");
    if (published || watermarks.size() != 8)
      throw std::runtime_error("BOOM semantic source roster is incomplete or already published");
    for (uint32_t hart = 0; hart < 4; ++hart) {
      for (const uint32_t id : {rvwmo::hart_producer_id(hart), rvwmo::memory_fabric_producer_id(hart)}) {
        const auto found = watermarks.find(id);
        if (found == watermarks.end() || found->second.cycle < completed_cycle)
          throw std::runtime_error("BOOM semantic source watermark does not close the scoring horizon");
      }
    }
    sql_exec(db, "COMMIT;");
    Statement samples(db, "SELECT id,window,producer,seq,cycle,phase,kind,hart,epoch,uid,subaccess,"
        "attempt,address,mask,lanes,tx_id,generation,byte_count,coalesced FROM samples "
        "ORDER BY window,cycle,phase,CASE WHEN producer >= 2147483648 THEN 0 ELSE 1 END,producer,seq,id");
    Statement retired(db, "SELECT pc,attempt,address,mask,lanes,flags FROM retired "
        "WHERE hart=? AND epoch=? AND uid=? AND subaccess=?");
    Statement squashed(db, "SELECT 1 FROM squashed WHERE hart=? AND epoch=? AND uid=?");
    Statement canceled(db, "SELECT 1 FROM canceled WHERE hart=? AND epoch=? AND uid=? "
        "AND subaccess=? AND attempt=?");
    Statement members(db, "SELECT ordinal,hart,epoch,uid,subaccess,attempt,generation FROM members "
        "WHERE sample_id=? ORDER BY ordinal");
    Statement add_load(db, "INSERT INTO emitted_loads VALUES(?,?,?,?,?)");
    Statement add_source(db, "INSERT INTO emitted_sources VALUES(?,?)");
    Statement add_transaction(db, "INSERT INTO emitted_transactions VALUES(?,?,?)");
    auto was_squashed = [&](uint64_t hart, uint64_t epoch, uint64_t uid) {
      squashed.bind(1, hart); squashed.bind(2, epoch); squashed.bind(3, uid);
      const bool result = squashed.next(); squashed.reset();
      return result;
    };
    auto was_canceled = [&](uint64_t hart, uint64_t epoch, uint64_t uid,
                            uint64_t subaccess, uint64_t attempt) {
      canceled.bind(1, hart); canceled.bind(2, epoch); canceled.bind(3, uid);
      canceled.bind(4, subaccess); canceled.bind(5, attempt);
      const bool result = canceled.next(); canceled.reset();
      return result;
    };
    auto retired_operation = [&](uint64_t hart, uint64_t epoch, uint64_t uid, uint64_t subaccess) {
      retired.bind(1, hart); retired.bind(2, epoch);
      retired.bind(3, uid); retired.bind(4, subaccess);
      return retired.next(); // The caller resets after reading the row (or missing row).
    };

    std::ostringstream chunk_rows, chunks;
    uint64_t window = 0, part = 0;
    size_t row_count = 0, chunk_count = 0, loads = 0, stores = 0;
    bool has_window = false;
    auto flush = [&] {
      if (!row_count) return;
      const uint64_t chunk_first = first_cycle + window * chunk_cycles;
      const uint64_t final_cycle = std::min(completed_cycle, chunk_first + chunk_cycles - 1);
      const auto filename = chunk_name(window, part++);
      std::ostringstream content;
      content << "{\"schema_version\":2,\"first_cycle\":" << chunk_first
              << ",\"last_cycle\":" << final_cycle << ",\"events\":["
              << chunk_rows.str() << "]}";
      const auto text = content.str();
      const auto path = (std::filesystem::path(output).parent_path() / filename).string();
      write_exclusive(path, text);
      published_chunks.push_back(path);
      if (chunk_count++) chunks << ',';
      chunks << "{\"first_cycle\":" << chunk_first << ",\"last_cycle\":" << final_cycle
             << ",\"part\":" << part - 1 << ",\"file\":" << json_string(filename)
             << ",\"event_count\":" << row_count << ",\"sha256\":" << json_string(sha256(text)) << '}';
      chunk_rows.str(""); chunk_rows.clear(); row_count = 0;
    };

    while (samples.next()) {
      const uint64_t sample_id = samples.number(0), sample_window = samples.number(1);
      const uint64_t producer = samples.number(2), sequence = samples.number(3);
      const uint64_t cycle = samples.number(4), phase = samples.number(5), kind = samples.number(6);
      if (cycle > completed_cycle)
        throw std::runtime_error("BOOM semantic accepted memory event is later than checked completion");
      const uint64_t hart = samples.number(7), epoch = samples.number(8), uid = samples.number(9);
      const uint64_t subaccess = samples.number(10), attempt = samples.number(11);
      const uint64_t address = samples.number(12), mask = samples.number(13), lanes = samples.number(14);
      if (!has_window || window != sample_window) {
        flush(); window = sample_window; part = 0; has_window = true;
      }
      std::ostringstream detail;
      if (kind == 1) {
        if (!retired_operation(hart, epoch, uid, subaccess)) { retired.reset(); continue; }
        const uint64_t pc = retired.number(0), final_attempt = retired.number(1);
        const uint64_t final_address = retired.number(2), final_mask = retired.number(3);
        const uint64_t final_lanes = retired.number(4), flags = retired.number(5);
        retired.reset();
        if (!(flags & 1) || (flags & (4 | 8 | 16)) || attempt != final_attempt) continue;
        if (was_squashed(hart, epoch, uid) || was_canceled(hart, epoch, uid, subaccess, attempt))
          throw std::runtime_error("BOOM semantic final load was canceled or squashed");
        if (hart >= 4 || producer != rvwmo::hart_producer_id(static_cast<uint32_t>(hart)) ||
            epoch != 1 || !uid || address != final_address ||
            mask != final_mask || lanes != final_lanes)
          throw std::runtime_error("BOOM semantic final load identity or address disagrees with retirement");
        checked_mask(static_cast<uint32_t>(mask), address, static_cast<uint32_t>(lanes));
        add_load.bind(1, hart); add_load.bind(2, epoch); add_load.bind(3, uid);
        add_load.bind(4, subaccess); add_load.bind(5, attempt); add_load.done();
        detail << "\"kind\":\"retired_load_sample\",\"retired\":true,\"hart\":" << hart
               << ",\"uid\":" << uid << ",\"attempt\":" << attempt << ",\"subaccess\":" << subaccess
               << ",\"pc\":" << pc << ",\"physical_address\":" << address
               << ",\"address_is_physical\":true,\"byte_mask\":" << mask;
        ++loads;
      } else if (kind == 2) {
        const uint64_t transaction = samples.number(15), generation = samples.number(16);
        const uint64_t byte_count = samples.number(17), coalesced = samples.number(18);
        members.bind(1, sample_id);
        if (!members.next() || !transaction || !generation || !byte_count || byte_count != lanes)
          throw std::runtime_error("BOOM semantic store lacks observed publication identity or byte lanes");
        if (hart >= 4 || producer != rvwmo::memory_fabric_producer_id(static_cast<uint32_t>(hart)))
          throw std::runtime_error("BOOM semantic store has an unsupported fabric producer");
        checked_mask(static_cast<uint32_t>(mask), address, static_cast<uint32_t>(lanes));
        detail << "\"kind\":\"store_publish_group\",\"hart\":" << hart
               << ",\"physical_address\":" << address
               << ",\"address_is_physical\":true,\"byte_mask\":" << mask
               << ",\"transaction_id\":" << transaction << ",\"allocation_generation\":" << generation
               << ",\"retired_members\":[";
        size_t member_count = 0;
        do {
          const uint64_t member_hart = members.number(1), member_epoch = members.number(2);
          const uint64_t member_uid = members.number(3), member_subaccess = members.number(4);
          const uint64_t member_attempt = members.number(5), memory_generation = members.number(6);
          if (!retired_operation(member_hart, member_epoch, member_uid, member_subaccess))
            throw std::runtime_error("BOOM semantic store member has no retired writer");
          const uint64_t final_attempt = retired.number(1), flags = retired.number(5);
          retired.reset();
          if (member_hart != hart || member_epoch != 1 || !member_uid || !memory_generation ||
              !(flags & 2) || (flags & 4) || final_attempt != member_attempt ||
              was_squashed(member_hart, member_epoch, member_uid) ||
              was_canceled(member_hart, member_epoch, member_uid, member_subaccess, member_attempt))
            throw std::runtime_error("BOOM semantic store member lacks its real retired writer attempt");
          if (member_count++) detail << ',';
          detail << "{\"hart\":" << hart << ",\"uid\":" << member_uid
                 << ",\"attempt\":" << member_attempt << ",\"subaccess\":" << member_subaccess
                 << ",\"memory_generation\":" << memory_generation << ",\"retired\":true}";
        } while (members.next());
        members.reset();
        if (member_count > 1 && !coalesced)
          throw std::runtime_error("BOOM semantic merged store lacks a physical group certificate");
        detail << ']';
        add_transaction.bind(1, producer); add_transaction.bind(2, transaction);
        add_transaction.bind(3, generation); add_transaction.done();
        ++stores;
      } else {
        throw std::runtime_error("BOOM semantic disk index has an unknown memory event");
      }
      add_source.bind(1, producer); add_source.bind(2, sequence); add_source.done();
      std::ostringstream row;
      row << "{\"producer_id\":" << json_string(producer_name(static_cast<uint32_t>(producer)))
          << ",\"producer_sequence\":" << sequence << ",\"source_cycle\":" << cycle
          << ",\"qualified_phase\":" << phase << ",\"source_clock\":" << json_string(kClock)
          << ",\"epoch\":1,\"final_attempt\":true," << detail.str() << '}';
      const auto text = row.str();
      if (row_count && (row_count >= kMaxChunkRows || chunk_rows.tellp() +
          static_cast<std::streamoff>(text.size()) > static_cast<std::streamoff>(kMaxChunkBytes - 256)))
        flush();
      if (row_count++) chunk_rows << ',';
      chunk_rows << text;
    }
    flush();

    std::ostringstream manifest;
    manifest << "{\"schema_version\":2,\"kind\":\"difftest-s-semantic.v2\",\"case_id\":" << json_string(case_id)
             << ",\"run_id\":" << json_string(run_id) << ",\"elf_sha256\":" << json_string(elf)
             << ",\"backend_sha256\":" << json_string(backend) << ",\"layout_sha256\":" << json_string(layout)
             << ",\"source_clock\":" << json_string(kClock)
             << ",\"epoch\":1,\"complete\":true,\"ordering_scope\":\"per_producer_only\""
             << ",\"first_cycle\":" << first_cycle << ",\"last_cycle\":" << completed_cycle
             << ",\"max_cycle_budget\":" << max_cycle_budget
             << ",\"chunk_cycles\":" << chunk_cycles << ",\"max_pair_gap_cycles\":" << max_pair_gap_cycles
             << ",\"line_bytes\":64,\"qualified_phases\":[0,1]"
             << ",\"required_producers\":[";
    for (uint32_t hart = 0; hart < 4; ++hart) {
      if (hart) manifest << ',';
      manifest << json_string("h" + std::to_string(hart));
    }
    for (uint32_t hart = 0; hart < 4; ++hart)
      manifest << ',' << json_string("f" + std::to_string(hart));
    manifest << "],\"producer_watermarks\":{";
    bool first = true;
    for (uint32_t hart = 0; hart < 4; ++hart) {
      for (const uint32_t producer : {rvwmo::hart_producer_id(hart), rvwmo::memory_fabric_producer_id(hart)}) {
        if (!first) manifest << ',';
        first = false;
        manifest << json_string(producer_name(producer)) << ":{\"epoch\":1,\"source_clock\":" << json_string(kClock)
                 << ",\"complete\":true,\"lossless\":true,\"closed_through_cycle\":"
                 << watermarks.at(producer).cycle << '}';
      }
    }
    manifest << "},\"chunks\":[" << chunks.str() << "]}";
    write_exclusive(output, manifest.str());
    published = true;
    std::fprintf(stderr, "DIFFTEST_S_SEMANTIC_EXPORT {\"loads\":%zu,\"stores\":%zu,\"last_cycle\":%llu,\"chunks\":%zu}\n",
        loads, stores, static_cast<unsigned long long>(completed_cycle), chunk_count);
  }
};

BoomSemanticLongExport::BoomSemanticLongExport(std::string output, std::string case_id, std::string run_id,
    std::string elf_sha256, std::string backend_sha256, std::string layout_sha256,
    uint64_t first_cycle, uint64_t max_cycle_budget, uint64_t chunk_cycles, uint64_t max_pair_gap_cycles)
    : impl_(std::make_unique<Impl>(std::move(output), std::move(case_id), std::move(run_id),
          std::move(elf_sha256), std::move(backend_sha256), std::move(layout_sha256), first_cycle, max_cycle_budget, chunk_cycles, max_pair_gap_cycles)) {}
BoomSemanticLongExport::~BoomSemanticLongExport() = default;
void BoomSemanticLongExport::observe(const rvwmo::RuntimeEvent &event) { impl_->observe(event); }
void BoomSemanticLongExport::publish(const std::map<uint32_t, rvwmo::SourcePosition> &watermarks,
    uint64_t checked_completed_cycle) {
  impl_->publish(watermarks, checked_completed_cycle);
}

} // namespace difftest_s::native
