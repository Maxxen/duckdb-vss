#include "hnsw/hnsw_index.hpp"

#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/execution/index/fixed_size_allocator.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "hnsw/hnsw.hpp"

namespace duckdb {

//------------------------------------------------------------------------------
// Linked Blocks
//------------------------------------------------------------------------------

class LinkedBlock {
public:
	static constexpr const idx_t BLOCK_SIZE = Storage::BLOCK_SIZE - sizeof(validity_t);
	static constexpr const idx_t BLOCK_DATA_SIZE = BLOCK_SIZE - sizeof(IndexPointer);
	static_assert(BLOCK_SIZE > sizeof(IndexPointer), "Block size must be larger than the size of an IndexPointer");

	IndexPointer next_block;
	char data[BLOCK_DATA_SIZE] = {0};
};

constexpr idx_t LinkedBlock::BLOCK_DATA_SIZE;
constexpr idx_t LinkedBlock::BLOCK_SIZE;

class LinkedBlockReader {
private:
	FixedSizeAllocator &allocator;

	IndexPointer root_pointer;
	IndexPointer current_pointer;
	idx_t position_in_block;

public:
	LinkedBlockReader(FixedSizeAllocator &allocator, IndexPointer root_pointer)
	    : allocator(allocator), root_pointer(root_pointer), current_pointer(root_pointer), position_in_block(0) {
	}

	void Reset() {
		current_pointer = root_pointer;
		position_in_block = 0;
	}

	idx_t ReadData(data_ptr_t buffer, idx_t length) {
		idx_t bytes_read = 0;
		while (bytes_read < length) {

			// TODO: Check if current pointer is valid

			auto block = allocator.Get<const LinkedBlock>(current_pointer, false);
			auto block_data = block->data;
			auto data_to_read = std::min(length - bytes_read, LinkedBlock::BLOCK_DATA_SIZE - position_in_block);
			std::memcpy(buffer + bytes_read, block_data + position_in_block, data_to_read);

			bytes_read += data_to_read;
			position_in_block += data_to_read;

			if (position_in_block == LinkedBlock::BLOCK_DATA_SIZE) {
				position_in_block = 0;
				current_pointer = block->next_block;
			}
		}

		return bytes_read;
	}
};

class LinkedBlockWriter {
private:
	FixedSizeAllocator &allocator;

	IndexPointer root_pointer;
	IndexPointer current_pointer;
	idx_t position_in_block;

public:
	LinkedBlockWriter(FixedSizeAllocator &allocator, IndexPointer root_pointer)
	    : allocator(allocator), root_pointer(root_pointer), current_pointer(root_pointer), position_in_block(0) {
	}

	void ClearCurrentBlock() {
		auto block = allocator.Get<LinkedBlock>(current_pointer, true);
		block->next_block.Clear();
		memset(block->data, 0, LinkedBlock::BLOCK_DATA_SIZE);
	}

	void Reset() {
		current_pointer = root_pointer;
		position_in_block = 0;
		ClearCurrentBlock();
	}

	void WriteData(const_data_ptr_t buffer, idx_t length) {
		idx_t bytes_written = 0;
		while (bytes_written < length) {
			auto block = allocator.Get<LinkedBlock>(current_pointer, true);
			auto block_data = block->data;
			auto data_to_write = std::min(length - bytes_written, LinkedBlock::BLOCK_DATA_SIZE - position_in_block);
			std::memcpy(block_data + position_in_block, buffer + bytes_written, data_to_write);

			bytes_written += data_to_write;
			position_in_block += data_to_write;

			if (position_in_block == LinkedBlock::BLOCK_DATA_SIZE) {
				position_in_block = 0;
				block->next_block = allocator.New();
				current_pointer = block->next_block;
				ClearCurrentBlock();
			}
		}
	}
};

//------------------------------------------------------------------------------
// HNSWIndex Methods
//------------------------------------------------------------------------------

// Constructor
HNSWIndex::HNSWIndex(const string &name, IndexConstraintType index_constraint_type, const vector<column_t> &column_ids,
                     TableIOManager &table_io_manager, const vector<unique_ptr<Expression>> &unbound_expressions,
                     AttachedDatabase &db, const case_insensitive_map_t<Value> &options, const IndexStorageInfo &info,
                     idx_t estimated_cardinality)
    : Index(name, TYPE_NAME, index_constraint_type, column_ids, table_io_manager, unbound_expressions, db) {

	if (index_constraint_type != IndexConstraintType::NONE) {
		throw NotImplementedException("HNSW indexes do not support unique or primary key constraints");
	}

	// Create a allocator for the linked blocks
	auto &block_manager = table_io_manager.GetIndexBlockManager();
	linked_block_allocator = make_uniq<FixedSizeAllocator>(sizeof(LinkedBlock), block_manager);

	// We only support one ARRAY column
	D_ASSERT(logical_types.size() == 1);
	auto &vector_type = logical_types[0];
	D_ASSERT(vector_type.id() == LogicalTypeId::ARRAY);

	// Get the size of the vector
	auto vector_size = ArrayType::GetSize(vector_type);
	auto vector_child_type = ArrayType::GetChildType(vector_type);

	// Get the scalar kind from the array child type. This parameter should be verified during binding.
	auto scalar_kind = unum::usearch::scalar_kind_t::f32_k;
	auto scalar_kind_val = SCALAR_KIND_MAP.find(static_cast<uint8_t>(vector_child_type.id()));
	if (scalar_kind_val != SCALAR_KIND_MAP.end()) {
		scalar_kind = scalar_kind_val->second;
	}

	// Try to get the vector metric from the options, this parameter should be verified during binding.
	auto metric_kind = unum::usearch::metric_kind_t::l2sq_k;
	auto metric_kind_opt = options.find("metric");
	if (metric_kind_opt != options.end()) {
		auto metric_kind_val = METRIC_KIND_MAP.find(metric_kind_opt->second.GetValue<string>());
		if (metric_kind_val != METRIC_KIND_MAP.end()) {
			metric_kind = metric_kind_val->second;
		}
	}

	// Create the usearch index
	unum::usearch::metric_punned_t metric(vector_size, metric_kind, scalar_kind);
	unum::usearch::index_dense_config_t config = {};

	// We dont need to do key lookups (id -> vector) in the index, DuckDB stores the vectors separately
	config.enable_key_lookups = false;

	auto ef_construction_opt = options.find("ef_construction");
	if (ef_construction_opt != options.end()) {
		config.expansion_add = ef_construction_opt->second.GetValue<int32_t>();
	}

	auto ef_search_opt = options.find("ef_search");
	if (ef_search_opt != options.end()) {
		config.expansion_search = ef_search_opt->second.GetValue<int32_t>();
	}

	auto m_opt = options.find("m");
	if (m_opt != options.end()) {
		config.connectivity = m_opt->second.GetValue<int32_t>();
		config.connectivity_base = config.connectivity * 2;
	}

	auto m0_opt = options.find("m0");
	if (m0_opt != options.end()) {
		config.connectivity_base = m0_opt->second.GetValue<int32_t>();
	}

	index = unum::usearch::index_dense_t::make(metric, config);

	auto lock = rwlock.GetExclusiveLock();
	// Is this a new index or an existing index?
	if (info.IsValid()) {
		// This is an old index that needs to be loaded

		// Set the root node
		root_block_ptr.Set(info.root);
		D_ASSERT(info.allocator_infos.size() == 1);
		linked_block_allocator->Init(info.allocator_infos[0]);

		// Is there anything to deserialize? We could have an empty index
		if(!info.allocator_infos[0].buffer_ids.empty()) {
			LinkedBlockReader reader(*linked_block_allocator, root_block_ptr);
			index.load_from_stream(
			    [&](void *data, size_t size) { return size == reader.ReadData(static_cast<data_ptr_t>(data), size); });
		}
	} else {
		index.reserve(MinValue(static_cast<idx_t>(32), estimated_cardinality));
	}
	index_size = index.size();
}

idx_t HNSWIndex::GetVectorSize() const {
	return index.dimensions();
}

string HNSWIndex::GetMetric() const {
	switch (index.metric().metric_kind()) {
	case unum::usearch::metric_kind_t::l2sq_k:
		return "l2sq";
	case unum::usearch::metric_kind_t::cos_k:
		return "cosine";
	case unum::usearch::metric_kind_t::ip_k:
		return "ip";
	default:
		throw InternalException("Unknown metric kind");
	}
}

bool HNSWIndex::IsDistanceFunction(const string &distance_function_name) {
	auto accepted_functions = {"array_distance", "array_cosine_similarity", "array_inner_product"};
	return std::find(accepted_functions.begin(), accepted_functions.end(), distance_function_name) !=
	       accepted_functions.end();
}

bool HNSWIndex::MatchesDistanceFunction(const string &distance_function_name) const {
	if (distance_function_name == "array_distance" &&
	    index.metric().metric_kind() == unum::usearch::metric_kind_t::l2sq_k) {
		return true;
	}
	if (distance_function_name == "array_cosine_similarity" &&
	    index.metric().metric_kind() == unum::usearch::metric_kind_t::cos_k) {
		return true;
	}
	if (distance_function_name == "array_inner_product" &&
	    index.metric().metric_kind() == unum::usearch::metric_kind_t::ip_k) {
		return true;
	}
	return false;
}

const case_insensitive_map_t<unum::usearch::metric_kind_t> HNSWIndex::METRIC_KIND_MAP = {
    {"l2sq", unum::usearch::metric_kind_t::l2sq_k},
    {"cosine", unum::usearch::metric_kind_t::cos_k},
    {"ip", unum::usearch::metric_kind_t::ip_k},
    /* TODO: Add the rest of these later
    {"divergence", unum::usearch::metric_kind_t::divergence_k},
    {"hamming", unum::usearch::metric_kind_t::hamming_k},
    {"jaccard", unum::usearch::metric_kind_t::jaccard_k},
    {"haversine", unum::usearch::metric_kind_t::haversine_k},
    {"pearson", unum::usearch::metric_kind_t::pearson_k},
    {"sorensen", unum::usearch::metric_kind_t::sorensen_k},
    {"tanimoto", unum::usearch::metric_kind_t::tanimoto_k}
     */
};

const unordered_map<uint8_t, unum::usearch::scalar_kind_t> HNSWIndex::SCALAR_KIND_MAP = {
    {static_cast<uint8_t>(LogicalTypeId::FLOAT), unum::usearch::scalar_kind_t::f32_k},
    {static_cast<uint8_t>(LogicalTypeId::DOUBLE), unum::usearch::scalar_kind_t::f64_k},
    {static_cast<uint8_t>(LogicalTypeId::TINYINT), unum::usearch::scalar_kind_t::i8_k},
    {static_cast<uint8_t>(LogicalTypeId::SMALLINT), unum::usearch::scalar_kind_t::i16_k},
    {static_cast<uint8_t>(LogicalTypeId::INTEGER), unum::usearch::scalar_kind_t::i32_k},
    {static_cast<uint8_t>(LogicalTypeId::BIGINT), unum::usearch::scalar_kind_t::i64_k},
    {static_cast<uint8_t>(LogicalTypeId::UTINYINT), unum::usearch::scalar_kind_t::u8_k},
    {static_cast<uint8_t>(LogicalTypeId::USMALLINT), unum::usearch::scalar_kind_t::u16_k},
    {static_cast<uint8_t>(LogicalTypeId::UINTEGER), unum::usearch::scalar_kind_t::u32_k},
    {static_cast<uint8_t>(LogicalTypeId::UBIGINT), unum::usearch::scalar_kind_t::u64_k}};

unique_ptr<HNSWIndexStats> HNSWIndex::GetStats() {
	auto lock = rwlock.GetExclusiveLock();
	auto result = make_uniq<HNSWIndexStats>();

	result->max_level = index.max_level();
	result->count = index.size();
	result->capacity = index.capacity();
	result->approx_size = index.memory_usage();

	for (idx_t i = 0; i < index.max_level(); i++) {
		result->level_stats.push_back(index.stats(i));
	}

	return result;
}

// Scan State
struct HNSWIndexScanState : public IndexScanState {
	idx_t current_row = 0;
	idx_t total_rows = 0;
	unique_array<row_t> row_ids = nullptr;
};

unique_ptr<IndexScanState> HNSWIndex::InitializeScan(float *query_vector, idx_t limit) {
	auto state = make_uniq<HNSWIndexScanState>();

	// Acquire a shared lock to search the index
	auto lock = rwlock.GetSharedLock();
	auto search_result = index.search(query_vector, limit);

	state->current_row = 0;
	state->total_rows = search_result.size();
	state->row_ids = make_uniq_array<row_t>(search_result.size());

	search_result.dump_to(reinterpret_cast<uint64_t *>(state->row_ids.get()));
	return std::move(state);
}

idx_t HNSWIndex::Scan(IndexScanState &state, Vector &result) {
	auto &scan_state = state.Cast<HNSWIndexScanState>();

	idx_t count = 0;
	auto row_ids = FlatVector::GetData<row_t>(result);

	// Push the row ids into the result vector, up to STANDARD_VECTOR_SIZE or the
	// end of the result set
	while (count < STANDARD_VECTOR_SIZE && scan_state.current_row < scan_state.total_rows) {
		row_ids[count++] = scan_state.row_ids[scan_state.current_row++];
	}

	return count;
}

void HNSWIndex::CommitDrop(IndexLock &index_lock) {
	// Acquire an exclusive lock to drop the index
	auto lock = rwlock.GetExclusiveLock();

	index.reset();
	index_size = 0;
	// TODO: Maybe we can drop these much earlier?
	linked_block_allocator->Reset();
	root_block_ptr.Clear();
}

void HNSWIndex::Construct(DataChunk &input, Vector &row_ids, idx_t thread_idx) {
	D_ASSERT(row_ids.GetType().InternalType() == ROW_TYPE);
	D_ASSERT(logical_types[0] == input.data[0].GetType());

	// Mark this index as dirty so we checkpoint it properly
	is_dirty = true;

	auto count = input.size();
	input.Flatten();

	auto &vec_vec = input.data[0];
	auto &vec_child_vec = ArrayVector::GetEntry(vec_vec);
	auto array_size = ArrayType::GetSize(vec_vec.GetType());

	auto vec_child_data = FlatVector::GetData<float>(vec_child_vec);
	auto rowid_data = FlatVector::GetData<row_t>(row_ids);

	// Check if we need to resize the index
	// We keep the size of the index in a separate atomic to avoid
	// locking exclusively when checking
	bool needs_resize = false;
	{
		auto lock = rwlock.GetSharedLock();
		if (index_size.fetch_add(count) + count > index.capacity()) {
			needs_resize = true;
		}
	}

	// We need to "upgrade" the lock to exclusive to resize the index
	if (needs_resize) {
		auto lock = rwlock.GetExclusiveLock();
		// Do we still need to resize?
		// Another thread might have resized it already
		auto size = index_size.load();
		if (size > index.capacity()) {
			// Add some extra space so that we don't need to resize too often
			index.reserve(NextPowerOfTwo(size));
		}
	}

	{
		// Now we can be sure that we have enough space in the index
		auto lock = rwlock.GetSharedLock();
		for (idx_t out_idx = 0; out_idx < count; out_idx++) {
			auto rowid = rowid_data[out_idx];
			auto result = index.add(rowid, vec_child_data + (out_idx * array_size), thread_idx);
			if (!result) {
				throw InternalException("Failed to add to the HNSW index: %s", result.error.what());
			}
		}
	}
}

void HNSWIndex::Compact() {
	// Mark this index as dirty so we checkpoint it properly
	is_dirty = true;

	// Acquire an exclusive lock to compact the index
	auto lock = rwlock.GetExclusiveLock();
	// Re-compact the index
	auto result = index.compact();
	if (!result) {
		throw InternalException("Failed to compact the HNSW index: %s", result.error.what());
	}

	index_size = index.size();
}

void HNSWIndex::Delete(IndexLock &lock, DataChunk &input, Vector &rowid_vec) {
	// Mark this index as dirty so we checkpoint it properly
	is_dirty = true;

	auto count = input.size();
	rowid_vec.Flatten(count);
	auto row_id_data = FlatVector::GetData<row_t>(rowid_vec);

	// For deleting from the index, we need an exclusive lock
	auto _lock = rwlock.GetExclusiveLock();

	for (idx_t i = 0; i < input.size(); i++) {
		auto result = index.remove(row_id_data[i]);
	}

	index_size = index.size();
}

ErrorData HNSWIndex::Insert(IndexLock &lock, DataChunk &input, Vector &rowid_vec) {
	Construct(input, rowid_vec, unum::usearch::index_dense_t::any_thread());
	return ErrorData {};
}

ErrorData HNSWIndex::Append(IndexLock &lock, DataChunk &entries, Vector &rowid_vec) {
	Construct(entries, rowid_vec, unum::usearch::index_dense_t::any_thread());
	return ErrorData {};
}

void HNSWIndex::VerifyAppend(DataChunk &chunk) {
	// There is nothing to verify here as we dont support constraints anyway
}

void HNSWIndex::VerifyAppend(DataChunk &chunk, ConflictManager &conflict_manager) {
	// There is nothing to verify here as we dont support constraints anyway
}

void HNSWIndex::PersistToDisk() {
	// Acquire an exclusive lock to persist the index
	auto lock = rwlock.GetExclusiveLock();

	// If there haven't been any changes, we don't need to rewrite the index again
	if (!is_dirty) {
		return;
	}

	// Write

	if (root_block_ptr.Get() == 0) {
		root_block_ptr = linked_block_allocator->New();
	}

	LinkedBlockWriter writer(*linked_block_allocator, root_block_ptr);
	writer.Reset();
	index.save_to_stream([&](const void *data, size_t size) {
		writer.WriteData(static_cast<const_data_ptr_t>(data), size);
		return true;
	});

	is_dirty = false;
}

IndexStorageInfo HNSWIndex::GetStorageInfo(const bool get_buffers) {

	PersistToDisk();

	IndexStorageInfo info;
	info.name = name;
	info.root = root_block_ptr.Get();

	if (!get_buffers) {
		// use the partial block manager to serialize all allocator data
		auto &block_manager = table_io_manager.GetIndexBlockManager();
		PartialBlockManager partial_block_manager(block_manager, CheckpointType::FULL_CHECKPOINT);
		linked_block_allocator->SerializeBuffers(partial_block_manager);
		partial_block_manager.FlushPartialBlocks();
	} else {
		info.buffers.push_back(linked_block_allocator->InitSerializationToWAL());
	}

	info.allocator_infos.push_back(linked_block_allocator->GetInfo());
	return info;
}

idx_t HNSWIndex::GetInMemorySize(IndexLock &state) {
	// TODO: This is not correct: its a lower bound, but it's a start
	return index.memory_usage();
}

bool HNSWIndex::MergeIndexes(IndexLock &state, Index &other_index) {
	throw NotImplementedException("HNSWIndex::MergeIndexes() not implemented");
}

void HNSWIndex::Vacuum(IndexLock &state) {
}

void HNSWIndex::CheckConstraintsForChunk(DataChunk &input, ConflictManager &conflict_manager) {
	throw NotImplementedException("HNSWIndex::CheckConstraintsForChunk() not implemented");
}

string HNSWIndex::VerifyAndToString(IndexLock &state, const bool only_verify) {
	throw NotImplementedException("HNSWIndex::VerifyAndToString() not implemented");
}

//------------------------------------------------------------------------------
// Register Index Type
//------------------------------------------------------------------------------
void HNSWModule::RegisterIndex(DatabaseInstance &db) {

	IndexType index_type;
	index_type.name = HNSWIndex::TYPE_NAME;

	index_type.create_instance = [](CreateIndexInput &input) -> unique_ptr<Index> {
		auto res = make_uniq<HNSWIndex>(input.name, input.constraint_type, input.column_ids, input.table_io_manager,
		                                input.unbound_expressions, input.db, input.options, input.storage_info);
		return std::move(res);
	};

	// Register the index type
	db.config.GetIndexTypes().RegisterIndexType(index_type);
}

} // namespace duckdb