/*
 * Copyright (c) 2021, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "cds/aotLinkedClassBulkLoader.hpp"
#include "cds/archiveHeapLoader.hpp"
#include "cds/cdsConfig.hpp"
#include "classfile/classLoader.hpp"
#include "classfile/classLoaderData.hpp"
#include "classfile/dictionary.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/vmClasses.hpp"
#include "classfile/vmSymbols.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "memory/metaspaceClosure.hpp"
#include "memory/universe.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/instanceRefKlass.hpp"
#include "oops/instanceStackChunkKlass.hpp"
#include "prims/jvmtiExport.hpp"
#include "runtime/globals.hpp"

InstanceKlass* vmClasses::_klasses[static_cast<int>(vmClassID::LIMIT)]
                                                 =  { nullptr /*, nullptr...*/ };
InstanceKlass* vmClasses::_box_klasses[T_VOID+1] =  { nullptr /*, nullptr...*/ };

bool vmClasses::is_loaded(InstanceKlass* klass) {
  return klass != nullptr && klass->is_loaded();
}

// Compact table of the vmSymbolIDs of all the VM classes (stored as short to save space)
static const short vm_class_name_ids[] = {
  #define VM_CLASS_NAME(name, symbol) ((short)VM_SYMBOL_ENUM_NAME(symbol)),
  VM_CLASSES_DO(VM_CLASS_NAME)
  #undef VM_CLASS_NAME
  0
};


#ifdef ASSERT
bool vmClasses::contain(Symbol* class_name) {
  int sid;
  for (int i = 0; (sid = vm_class_name_ids[i]) != 0; i++) {
    Symbol* symbol = vmSymbols::symbol_at(vmSymbols::as_SID(sid));
    if (class_name == symbol) {
      return true;
    }
  }
  return false;
}

bool vmClasses::contain(Klass* k) {
  return contain(k->name());
}
#endif

bool vmClasses::resolve(vmClassID id, TRAPS) {
  InstanceKlass** klassp = &_klasses[as_int(id)];

#if INCLUDE_CDS
  if (CDSConfig::is_using_archive() && !JvmtiExport::should_post_class_prepare()) {
    InstanceKlass* k = *klassp;
    assert(k->defined_by_boot_loader(), "must be");

    ClassLoaderData* loader_data = ClassLoaderData::the_null_class_loader_data();
    resolve_shared_class(k, loader_data, Handle(), CHECK_false);
    return true;
  }
#endif // INCLUDE_CDS

  if (!is_loaded(*klassp)) {
    int sid = vm_class_name_ids[as_int(id)];
    Symbol* symbol = vmSymbols::symbol_at(vmSymbols::as_SID(sid));
    Klass* k = SystemDictionary::resolve_or_fail(symbol, true, CHECK_false);
    (*klassp) = InstanceKlass::cast(k);
  }
  return ((*klassp) != nullptr);
}

void vmClasses::resolve_until(vmClassID limit_id, vmClassID &start_id, TRAPS) {
  assert((int)start_id <= (int)limit_id, "IDs are out of order!");
  for (auto id : EnumRange<vmClassID>{start_id, limit_id}) { // (inclusive start, exclusive end)
    resolve(id, CHECK);
  }

  // move the starting value forward to the limit:
  start_id = limit_id;
}

void vmClasses::resolve_all(TRAPS) {
  assert(!Object_klass_loaded(), "well-known classes should only be initialized once");

  // Create the ModuleEntry for java.base.  This call needs to be done here,
  // after vmSymbols::initialize() is called but before any classes are pre-loaded.
  ClassLoader::classLoader_init2(THREAD);

  // Preload commonly used klasses
  vmClassID scan = vmClassID::FIRST;
  // first do Object, then String, Class
  resolve_through(VM_CLASS_ID(Object_klass), scan, CHECK);
  CollectedHeap::set_filler_object_klass(vmClasses::Object_klass());
#if INCLUDE_CDS
  if (CDSConfig::is_using_archive()) {
    // It's unsafe to access the archived heap regions before they
    // are fixed up, so we must do the fixup as early as possible
    // before the archived java objects are accessed by functions
    // such as java_lang_Class::restore_archived_mirror and
    // ConstantPool::restore_unshareable_info (restores the archived
    // resolved_references array object).
    //
    // ArchiveHeapLoader::fixup_regions fills the empty
    // spaces in the archived heap regions and may use
    // vmClasses::Object_klass(), so we can do this only after
    // Object_klass is resolved. See the above resolve_through()
    // call. No mirror objects are accessed/restored in the above call.
    // Mirrors are restored after java.lang.Class is loaded.
    ArchiveHeapLoader::fixup_region();

    // Initialize the constant pool for the Object_class
    assert(Object_klass()->is_shared(), "must be");
    Object_klass()->constants()->restore_unshareable_info(CHECK);
    resolve_through(VM_CLASS_ID(Class_klass), scan, CHECK);
  } else
#endif
  {
    resolve_through(VM_CLASS_ID(Class_klass), scan, CHECK);
  }

  assert(vmClasses::Object_klass() != nullptr, "well-known classes should now be initialized");

  java_lang_Object::register_natives(CHECK);

  // Calculate offsets for String and Class classes since they are loaded and
  // can be used after this point. These are no-op when CDS is enabled.
  java_lang_String::compute_offsets();
  java_lang_Class::compute_offsets();

  // Fixup mirrors for classes loaded before java.lang.Class.
  Universe::initialize_basic_type_mirrors(CHECK);
  Universe::fixup_mirrors(CHECK);

  if (CDSConfig::is_using_archive()) {
    // These should already have been initialized during CDS dump.
    assert(vmClasses::Reference_klass()->reference_type() == REF_NONE, "sanity");
    assert(vmClasses::SoftReference_klass()->reference_type() == REF_SOFT, "sanity");
    assert(vmClasses::WeakReference_klass()->reference_type() == REF_WEAK, "sanity");
    assert(vmClasses::FinalReference_klass()->reference_type() == REF_FINAL, "sanity");
    assert(vmClasses::PhantomReference_klass()->reference_type() == REF_PHANTOM, "sanity");
  } else {
    // If CDS is not enabled, the references classes must be initialized in
    // this order before the rest of the vmClasses can be resolved.
    resolve_through(VM_CLASS_ID(Reference_klass), scan, CHECK);

    // The offsets for jlr.Reference must be computed before
    // InstanceRefKlass::update_nonstatic_oop_maps is called. That function uses
    // the offsets to remove the referent and discovered fields from the oop maps,
    // as they are treated in a special way by the GC. Removing these oops from the
    // oop maps must be done before the usual subclasses of jlr.Reference are loaded.
    java_lang_ref_Reference::compute_offsets();

    // Preload ref klasses and set reference types
    InstanceRefKlass::update_nonstatic_oop_maps(vmClasses::Reference_klass());

    resolve_through(VM_CLASS_ID(PhantomReference_klass), scan, CHECK);
  }

  resolve_until(vmClassID::LIMIT, scan, CHECK);

  CollectedHeap::set_filler_object_klass(vmClasses::FillerObject_klass());

  _box_klasses[T_BOOLEAN] = vmClasses::Boolean_klass();
  _box_klasses[T_CHAR]    = vmClasses::Character_klass();
  _box_klasses[T_FLOAT]   = vmClasses::Float_klass();
  _box_klasses[T_DOUBLE]  = vmClasses::Double_klass();
  _box_klasses[T_BYTE]    = vmClasses::Byte_klass();
  _box_klasses[T_SHORT]   = vmClasses::Short_klass();
  _box_klasses[T_INT]     = vmClasses::Integer_klass();
  _box_klasses[T_LONG]    = vmClasses::Long_klass();

#ifdef ASSERT
  if (CDSConfig::is_using_archive()) {
    JVMTI_ONLY(assert(JvmtiExport::is_early_phase(),
                      "All well known classes must be resolved in JVMTI early phase"));
    for (auto id : EnumRange<vmClassID>{}) {
      InstanceKlass* k = _klasses[as_int(id)];
      assert(k->is_shared(), "must not be replaced by JVMTI class file load hook");
    }
  }
#endif

  InstanceStackChunkKlass::init_offset_of_stack();
  if (CDSConfig::is_using_aot_linked_classes()) {
    AOTLinkedClassBulkLoader::load_javabase_classes(THREAD);
  }
}

#if INCLUDE_CDS

void vmClasses::resolve_shared_class(InstanceKlass* klass, ClassLoaderData* loader_data, Handle domain, TRAPS) {
  assert(!Universe::is_fully_initialized(), "We can make short cuts only during VM initialization");
  assert(klass->is_shared(), "Must be shared class");
  if (klass->class_loader_data() != nullptr) {
    return;
  }

  // add super and interfaces first
  Klass* super = klass->super();
  if (super != nullptr && super->class_loader_data() == nullptr) {
    assert(super->is_instance_klass(), "Super should be instance klass");
    resolve_shared_class(InstanceKlass::cast(super), loader_data, domain, CHECK);
  }

  Array<InstanceKlass*>* ifs = klass->local_interfaces();
  for (int i = 0; i < ifs->length(); i++) {
    InstanceKlass* ik = ifs->at(i);
    if (ik->class_loader_data()  == nullptr) {
      resolve_shared_class(ik, loader_data, domain, CHECK);
    }
  }

  klass->restore_unshareable_info(loader_data, domain, nullptr, THREAD);
  SystemDictionary::load_shared_class_misc(klass, loader_data);
  Dictionary* dictionary = loader_data->dictionary();
  dictionary->add_klass(THREAD, klass->name(), klass);
  klass->add_to_hierarchy(THREAD);
  assert(klass->is_loaded(), "Must be in at least loaded state");
}

#endif // INCLUDE_CDS

// Tells if a given klass is a box (wrapper class, such as java.lang.Integer).
// If so, returns the basic type it holds.  If not, returns T_OBJECT.
BasicType vmClasses::box_klass_type(Klass* k) {
  assert(k != nullptr, "");
  for (int i = T_BOOLEAN; i < T_VOID+1; i++) {
    if (_box_klasses[i] == k)
      return (BasicType)i;
  }
  return T_OBJECT;
}
