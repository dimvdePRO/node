// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/snapshot/builtin-serializer.h"

#include "src/interpreter/interpreter.h"
#include "src/objects-inl.h"
#include "src/snapshot/startup-serializer.h"

namespace v8 {
namespace internal {

using interpreter::Bytecode;
using interpreter::Bytecodes;
using interpreter::OperandScale;

BuiltinSerializer::BuiltinSerializer(Isolate* isolate,
                                     StartupSerializer* startup_serializer)
    : Serializer(isolate), startup_serializer_(startup_serializer) {}

BuiltinSerializer::~BuiltinSerializer() {
  OutputStatistics("BuiltinSerializer");
}

void BuiltinSerializer::SerializeBuiltinsAndHandlers() {
  // Serialize builtins.

  STATIC_ASSERT(0 == BSU::kFirstBuiltinIndex);

  for (int i = 0; i < BSU::kNumberOfBuiltins; i++) {
    SetBuiltinOffset(i, sink_.Position());
    SerializeBuiltin(isolate()->builtins()->builtin(i));
  }

  // Serialize bytecode handlers.

  STATIC_ASSERT(BSU::kNumberOfBuiltins == BSU::kFirstHandlerIndex);

  BSU::ForEachBytecode([=](Bytecode bytecode, OperandScale operand_scale) {
    SetHandlerOffset(bytecode, operand_scale, sink_.Position());
    if (!Bytecodes::BytecodeHasHandler(bytecode, operand_scale)) return;

    SerializeHandler(
        isolate()->interpreter()->GetBytecodeHandler(bytecode, operand_scale));
  });

  STATIC_ASSERT(BSU::kFirstHandlerIndex + BSU::kNumberOfHandlers ==
                BSU::kNumberOfCodeObjects);

  // The DeserializeLazy handlers are serialized by the StartupSerializer
  // during strong root iteration.

  DCHECK(isolate()->heap()->deserialize_lazy_handler()->IsCode());
  DCHECK(isolate()->heap()->deserialize_lazy_handler_wide()->IsCode());
  DCHECK(isolate()->heap()->deserialize_lazy_handler_extra_wide()->IsCode());

  // Pad with kNop since GetInt() might read too far.
  Pad();

  // Append the offset table. During deserialization, the offset table is
  // extracted by BuiltinSnapshotData.
  const byte* data = reinterpret_cast<const byte*>(&code_offsets_[0]);
  int data_length = static_cast<int>(sizeof(code_offsets_));
  sink_.PutRaw(data, data_length, "BuiltinOffsets");
}

void BuiltinSerializer::VisitRootPointers(Root root, Object** start,
                                          Object** end) {
  UNREACHABLE();  // We iterate manually in SerializeBuiltins.
}

void BuiltinSerializer::SerializeBuiltin(Code* code) {
  DCHECK_GE(code->builtin_index(), 0);

  // All builtins are serialized unconditionally when the respective builtin is
  // reached while iterating the builtins list. A builtin seen at any other
  // time (e.g. startup snapshot creation, or while iterating a builtin code
  // object during builtin serialization) is serialized by reference - see
  // BuiltinSerializer::SerializeObject below.
  ObjectSerializer object_serializer(this, code, &sink_, kPlain,
                                     kStartOfObject);
  object_serializer.Serialize();
}

void BuiltinSerializer::SerializeHandler(Code* code) {
  DCHECK(ObjectIsBytecodeHandler(code));
  ObjectSerializer object_serializer(this, code, &sink_, kPlain,
                                     kStartOfObject);
  object_serializer.Serialize();
}

void BuiltinSerializer::SerializeObject(HeapObject* o, HowToCode how_to_code,
                                        WhereToPoint where_to_point, int skip) {
  DCHECK(!o->IsSmi());

  // Roots can simply be serialized as root references.
  int root_index = root_index_map()->Lookup(o);
  if (root_index != RootIndexMap::kInvalidRootIndex) {
    DCHECK(startup_serializer_->root_has_been_serialized(root_index));
    PutRoot(root_index, o, how_to_code, where_to_point, skip);
    return;
  }

  // Builtins are serialized using a dedicated bytecode. We only reach this
  // point if encountering a Builtin e.g. while iterating the body of another
  // builtin.
  if (SerializeBuiltinReference(o, how_to_code, where_to_point, skip)) return;

  // Embedded objects are serialized as part of the partial snapshot cache.
  // Currently we expect to see:
  // * Code: Jump targets.
  // * ByteArrays: Relocation infos.
  // * FixedArrays: Handler tables.
  // * Strings: CSA_ASSERTs in debug builds, various other string constants.
  // * HeapNumbers: Embedded constants.
  // TODO(6624): Jump targets should never trigger content serialization, it
  // should always result in a reference instead. Reloc infos and handler
  // tables should not end up in the partial snapshot cache.

  FlushSkip(skip);

  int cache_index = startup_serializer_->PartialSnapshotCacheIndex(o);
  sink_.Put(kPartialSnapshotCache + how_to_code + where_to_point,
            "PartialSnapshotCache");
  sink_.PutInt(cache_index, "partial_snapshot_cache_index");
}

void BuiltinSerializer::SetBuiltinOffset(int builtin_id, uint32_t offset) {
  DCHECK(Builtins::IsBuiltinId(builtin_id));
  DCHECK(BSU::IsBuiltinIndex(builtin_id));
  code_offsets_[builtin_id] = offset;
}

void BuiltinSerializer::SetHandlerOffset(Bytecode bytecode,
                                         OperandScale operand_scale,
                                         uint32_t offset) {
  const int index = BSU::BytecodeToIndex(bytecode, operand_scale);
  DCHECK(BSU::IsHandlerIndex(index));
  code_offsets_[index] = offset;
}

}  // namespace internal
}  // namespace v8
