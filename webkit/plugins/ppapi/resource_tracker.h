// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WEBKIT_PLUGINS_PPAPI_RESOURCE_TRACKER_H_
#define WEBKIT_PLUGINS_PPAPI_RESOURCE_TRACKER_H_

#include <map>
#include <set>
#include <utility>

#include "base/basictypes.h"
#include "base/gtest_prod_util.h"
#include "base/hash_tables.h"
#include "base/memory/linked_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_ptr.h"
#include "ppapi/c/pp_instance.h"
#include "ppapi/c/pp_module.h"
#include "ppapi/c/pp_resource.h"
#include "ppapi/proxy/interface_id.h"
#include "ppapi/shared_impl/function_group_base.h"
#include "ppapi/shared_impl/tracker_base.h"

typedef struct NPObject NPObject;

namespace webkit {
namespace ppapi {

class ObjectVar;
class PluginInstance;
class PluginModule;
class Resource;
class ResourceTrackerTest;
class Var;

// This class maintains a global list of all live pepper resources. It allows
// us to check resource ID validity and to map them to a specific module.
//
// This object is NOT threadsafe.
class ResourceTracker : public ::ppapi::TrackerBase {
 public:
  // Returns the pointer to the singleton object.
  static ResourceTracker* Get();

  // PP_Resources --------------------------------------------------------------

  // The returned pointer will be NULL if there is no resource. Note that this
  // return value is a scoped_refptr so that we ensure the resource is valid
  // from the point of the lookup to the point that the calling code needs it.
  // Otherwise, the plugin could Release() the resource on another thread and
  // the object will get deleted out from under us.
  scoped_refptr<Resource> GetResource(PP_Resource res) const;

  // Increment resource's plugin refcount. See ResourceAndRefCount comments
  // below.
  bool AddRefResource(PP_Resource res);
  bool UnrefResource(PP_Resource res);

  // Returns the number of resources associated with this module.
  uint32 GetLiveObjectsForInstance(PP_Instance instance) const;

  // ResourceTrackerBase.
  virtual ::ppapi::ResourceObjectBase* GetResourceAPI(
      PP_Resource res) OVERRIDE;
  virtual ::ppapi::FunctionGroupBase* GetFunctionAPI(
      PP_Instance pp_instance,
      pp::proxy::InterfaceID id) OVERRIDE;
  virtual PP_Instance GetInstanceForResource(PP_Resource resource) OVERRIDE;

  // PP_Vars -------------------------------------------------------------------

  scoped_refptr<Var> GetVar(int32 var_id) const;

  bool AddRefVar(int32 var_id);
  bool UnrefVar(int32 var_id);

  // Tracks all live ObjectVar. This is so we can map between instance +
  // NPObject and get the ObjectVar corresponding to it. This Add/Remove
  // function is called by the ObjectVar when it is created and
  // destroyed.
  void AddNPObjectVar(ObjectVar* object_var);
  void RemoveNPObjectVar(ObjectVar* object_var);

  // Looks up a previously registered ObjectVar for the given NPObject and
  // instance. Returns NULL if there is no ObjectVar corresponding to the given
  // NPObject for the given instance. See AddNPObjectVar above.
  ObjectVar* ObjectVarForNPObject(PP_Instance instance, NPObject* np_object);

  // PP_Modules ----------------------------------------------------------------

  // Adds a new plugin module to the list of tracked module, and returns a new
  // module handle to identify it.
  PP_Module AddModule(PluginModule* module);

  // Called when a plugin modulde was deleted and should no longer be tracked.
  // The given handle should be one generated by AddModule.
  void ModuleDeleted(PP_Module module);

  // Returns a pointer to the plugin modulde object associated with the given
  // modulde handle. The return value will be NULL if the handle is invalid.
  PluginModule* GetModule(PP_Module module);

  // PP_Instances --------------------------------------------------------------

  // Adds a new plugin instance to the list of tracked instances, and returns a
  // new instance handle to identify it.
  PP_Instance AddInstance(PluginInstance* instance);

  // Called when a plugin instance was deleted and should no longer be tracked.
  // The given handle should be one generated by AddInstance.
  void InstanceDeleted(PP_Instance instance);

  void InstanceCrashed(PP_Instance instance);

  // Returns a pointer to the plugin instance object associated with the given
  // instance handle. The return value will be NULL if the handle is invalid or
  // if the instance has crashed.
  PluginInstance* GetInstance(PP_Instance instance);

 private:
  friend class Resource;
  friend class ResourceTrackerTest;
  friend class Var;

  typedef std::set<PP_Resource> ResourceSet;

  // Per-instance data we track.
  struct InstanceData;

  // Prohibit creation other then by the Singleton class.
  ResourceTracker();
  ~ResourceTracker();

  // Called when a new resource is created and associates it with its
  // PluginInstance.
  void ResourceCreated(Resource* resource, PluginInstance* instance);

  // Removes a resource from the resource map.
  void ResourceDestroyed(Resource* resource);

  // Adds the given resource to the tracker and assigns it a resource ID and
  // refcount of 1. The assigned resource ID will be returned. Used only by the
  // Resource class.
  PP_Resource AddResource(Resource* resource);

  // The same as AddResource but for Var, and returns the new Var ID.
  int32 AddVar(Var* var);

  // Force frees all vars and resources associated with the given instance.
  // If delete_instance is true, the instance tracking information will also
  // be deleted.
  void CleanupInstanceData(PP_Instance instance, bool delete_instance);

  // Overrides the singleton object. This is used for tests which want to
  // specify their own tracker (otherwise, you can get cross-talk between
  // tests since the data will live into the subsequent tests).
  static void SetSingletonOverride(ResourceTracker* tracker);
  static void ClearSingletonOverride();

  // The lazy-initialized global instance of this object. This is created in
  // ::Get() if there is no singleton_override_ specified.
  //
  // It would be nice to use LazyInstance for this since it manages the
  // creation properly, and also cleans up on shutdown. However, the shutdown
  // cleanup causes problems in some cases.
  //
  // For example, say the browser crashes or is killed. The renderer then
  // decides to exit. Normally resources are bound to an instance and are
  // cleaned up when WebKit deletes the instance (when you go to a different
  // page or close that view). In this case, WebKit doesn't clean up. If the
  // ResourceTracker was cleaned up by the AtExitManager (which would be the
  // case with LazyInstance/Singleton) then we'd try to call up to the renderer
  // layer via the delegate, which may be in a random state of shutdown.
  //
  // So effectively our rule is: any resources still around at shutdown are
  // associated with leaked plugins in WebKit, so it's also OK to leak those
  // resources from here (avoiding the shutdown race).
  static ResourceTracker* global_tracker_;

  // See SetSingletonOverride above.
  static ResourceTracker* singleton_override_;

  // Last assigned resource & var ID.
  PP_Resource last_resource_id_;
  int32 last_var_id_;

  // For each PP_Resource, keep the Resource* (as refptr) and plugin use count.
  // This use count is different then Resource's RefCount, and is manipulated
  // using this AddRefResource/UnrefResource. When it drops to zero, we just
  // remove the resource from this resource tracker, but the resource object
  // will be alive so long as some scoped_refptr still holds it's
  // reference. This prevents plugins from forcing destruction of Resource
  // objects.
  typedef std::pair<scoped_refptr<Resource>, size_t> ResourceAndRefCount;
  typedef base::hash_map<PP_Resource, ResourceAndRefCount> ResourceMap;
  ResourceMap live_resources_;

  // Like ResourceAndRefCount but for vars, which are associated with modules.
  typedef std::pair<scoped_refptr<Var>, size_t> VarAndRefCount;
  typedef base::hash_map<int32, VarAndRefCount> VarMap;
  VarMap live_vars_;

  // Tracks all live instances and their associated data.
  typedef std::map<PP_Instance, linked_ptr<InstanceData> > InstanceMap;
  InstanceMap instance_map_;

  // Tracks all live modules. The pointers are non-owning, the PluginModule
  // destructor will notify us when the module is deleted.
  typedef std::map<PP_Module, PluginModule*> ModuleMap;
  ModuleMap module_map_;

  DISALLOW_COPY_AND_ASSIGN(ResourceTracker);
};

}  // namespace ppapi
}  // namespace webkit

#endif  // WEBKIT_PLUGINS_PPAPI_RESOURCE_TRACKER_H_
