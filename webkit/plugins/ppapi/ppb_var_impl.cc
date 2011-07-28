// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "webkit/plugins/ppapi/ppb_var_impl.h"

#include <limits>

#include "ppapi/c/dev/ppb_var_deprecated.h"
#include "ppapi/c/ppb_var.h"
#include "ppapi/c/pp_var.h"
#include "third_party/WebKit/Source/WebKit/chromium/public/WebBindings.h"
#include "webkit/plugins/ppapi/common.h"
#include "webkit/plugins/ppapi/npapi_glue.h"
#include "webkit/plugins/ppapi/plugin_module.h"
#include "webkit/plugins/ppapi/plugin_object.h"
#include "webkit/plugins/ppapi/ppapi_plugin_instance.h"
#include "webkit/plugins/ppapi/resource_tracker.h"
#include "webkit/plugins/ppapi/var.h"
#include "v8/include/v8.h"

using WebKit::WebBindings;

namespace webkit {
namespace ppapi {

namespace {

const char kInvalidObjectException[] = "Error: Invalid object";
const char kInvalidPropertyException[] = "Error: Invalid property";
const char kInvalidValueException[] = "Error: Invalid value";
const char kUnableToGetPropertyException[] = "Error: Unable to get property";
const char kUnableToSetPropertyException[] = "Error: Unable to set property";
const char kUnableToRemovePropertyException[] =
    "Error: Unable to remove property";
const char kUnableToGetAllPropertiesException[] =
    "Error: Unable to get all properties";
const char kUnableToCallMethodException[] = "Error: Unable to call method";
const char kUnableToConstructException[] = "Error: Unable to construct";

// ---------------------------------------------------------------------------
// Utilities

// Converts the given PP_Var to an NPVariant, returning true on success.
// False means that the given variant is invalid. In this case, the result
// NPVariant will be set to a void one.
//
// The contents of the PP_Var will NOT be copied, so you need to ensure that
// the PP_Var remains valid while the resultant NPVariant is in use.
bool PPVarToNPVariantNoCopy(PP_Var var, NPVariant* result) {
  switch (var.type) {
    case PP_VARTYPE_UNDEFINED:
      VOID_TO_NPVARIANT(*result);
      break;
    case PP_VARTYPE_NULL:
      NULL_TO_NPVARIANT(*result);
      break;
    case PP_VARTYPE_BOOL:
      BOOLEAN_TO_NPVARIANT(var.value.as_bool, *result);
      break;
    case PP_VARTYPE_INT32:
      INT32_TO_NPVARIANT(var.value.as_int, *result);
      break;
    case PP_VARTYPE_DOUBLE:
      DOUBLE_TO_NPVARIANT(var.value.as_double, *result);
      break;
    case PP_VARTYPE_STRING: {
      scoped_refptr<StringVar> string(StringVar::FromPPVar(var));
      if (!string) {
        VOID_TO_NPVARIANT(*result);
        return false;
      }
      const std::string& value = string->value();
      STRINGN_TO_NPVARIANT(value.c_str(), value.size(), *result);
      break;
    }
    case PP_VARTYPE_OBJECT: {
      scoped_refptr<ObjectVar> object(ObjectVar::FromPPVar(var));
      if (!object) {
        VOID_TO_NPVARIANT(*result);
        return false;
      }
      OBJECT_TO_NPVARIANT(object->np_object(), *result);
      break;
    }
    default:
      VOID_TO_NPVARIANT(*result);
      return false;
  }
  return true;
}

// ObjectAccessorTryCatch ------------------------------------------------------

// Automatically sets up a TryCatch for accessing the object identified by the
// given PP_Var. The module from the object will be used for the exception
// strings generated by the TryCatch.
//
// This will automatically retrieve the ObjectVar from the object and throw
// an exception if it's invalid. At the end of construction, if there is no
// exception, you know that there is no previously set exception, that the
// object passed in is valid and ready to use (via the object() getter), and
// that the TryCatch's pp_module() getter is also set up properly and ready to
// use.
class ObjectAccessorTryCatch : public TryCatch {
 public:
  ObjectAccessorTryCatch(PP_Var object, PP_Var* exception)
      : TryCatch(0, exception),
        object_(ObjectVar::FromPPVar(object)) {
    if (!object_) {
      // No object or an invalid object was given. This means we have no module
      // to associated with the exception text, so use the magic invalid object
      // exception.
      SetInvalidObjectException();
    } else {
      // When the object is valid, we have a valid module to associate
      set_pp_module(object_->pp_module());
    }
  }

  ObjectVar* object() { return object_.get(); }

  PluginInstance* GetPluginInstance() {
    return ResourceTracker::Get()->GetInstance(object()->pp_instance());
  }

 protected:
  scoped_refptr<ObjectVar> object_;

  DISALLOW_COPY_AND_ASSIGN(ObjectAccessorTryCatch);
};

// ObjectAccessiorWithIdentifierTryCatch ---------------------------------------

// Automatically sets up a TryCatch for accessing the identifier on the given
// object. This just extends ObjectAccessorTryCatch to additionally convert
// the given identifier to an NPIdentifier and validate it, throwing an
// exception if it's invalid.
//
// At the end of construction, if there is no exception, you know that there is
// no previously set exception, that the object passed in is valid and ready to
// use (via the object() getter), that the identifier is valid and ready to
// use (via the identifier() getter), and that the TryCatch's pp_module() getter
// is also set up properly and ready to use.
class ObjectAccessorWithIdentifierTryCatch : public ObjectAccessorTryCatch {
 public:
  ObjectAccessorWithIdentifierTryCatch(PP_Var object,
                                       PP_Var identifier,
                                       PP_Var* exception)
      : ObjectAccessorTryCatch(object, exception),
        identifier_(0) {
    if (!has_exception()) {
      identifier_ = PPVarToNPIdentifier(identifier);
      if (!identifier_)
        SetException(kInvalidPropertyException);
    }
  }

  NPIdentifier identifier() const { return identifier_; }

 private:
  NPIdentifier identifier_;

  DISALLOW_COPY_AND_ASSIGN(ObjectAccessorWithIdentifierTryCatch);
};

// PPB_Var methods -------------------------------------------------------------

PP_Var VarFromUtf8(PP_Module module, const char* data, uint32_t len) {
  return StringVar::StringToPPVar(module, data, len);
}

const char* VarToUtf8(PP_Var var, uint32_t* len) {
  scoped_refptr<StringVar> str(StringVar::FromPPVar(var));
  if (!str) {
    *len = 0;
    return NULL;
  }
  *len = static_cast<uint32_t>(str->value().size());
  if (str->value().empty())
    return "";  // Don't return NULL on success.
  return str->value().data();
}

PP_Bool HasProperty(PP_Var var,
                    PP_Var name,
                    PP_Var* exception) {
  ObjectAccessorWithIdentifierTryCatch accessor(var, name, exception);
  if (accessor.has_exception())
    return PP_FALSE;
  return BoolToPPBool(WebBindings::hasProperty(NULL,
                                               accessor.object()->np_object(),
                                               accessor.identifier()));
}

bool HasPropertyDeprecated(PP_Var var,
                           PP_Var name,
                           PP_Var* exception) {
  return PPBoolToBool(HasProperty(var, name, exception));
}

bool HasMethodDeprecated(PP_Var var,
                         PP_Var name,
                         PP_Var* exception) {
  ObjectAccessorWithIdentifierTryCatch accessor(var, name, exception);
  if (accessor.has_exception())
    return false;
  return WebBindings::hasMethod(NULL, accessor.object()->np_object(),
                                accessor.identifier());
}

PP_Var GetProperty(PP_Var var,
                   PP_Var name,
                   PP_Var* exception) {
  ObjectAccessorWithIdentifierTryCatch accessor(var, name, exception);
  if (accessor.has_exception())
    return PP_MakeUndefined();

  NPVariant result;
  if (!WebBindings::getProperty(NULL, accessor.object()->np_object(),
                                accessor.identifier(), &result)) {
    // An exception may have been raised.
    accessor.SetException(kUnableToGetPropertyException);
    return PP_MakeUndefined();
  }

  PP_Var ret = NPVariantToPPVar(accessor.GetPluginInstance(), &result);
  WebBindings::releaseVariantValue(&result);
  return ret;
}

void EnumerateProperties(PP_Var var,
                         uint32_t* property_count,
                         PP_Var** properties,
                         PP_Var* exception) {
  *properties = NULL;
  *property_count = 0;

  ObjectAccessorTryCatch accessor(var, exception);
  if (accessor.has_exception())
    return;

  NPIdentifier* identifiers = NULL;
  uint32_t count = 0;
  if (!WebBindings::enumerate(NULL, accessor.object()->np_object(),
                              &identifiers, &count)) {
    accessor.SetException(kUnableToGetAllPropertiesException);
    return;
  }

  if (count == 0)
    return;

  *property_count = count;
  *properties = static_cast<PP_Var*>(malloc(sizeof(PP_Var) * count));
  for (uint32_t i = 0; i < count; ++i) {
    (*properties)[i] = NPIdentifierToPPVar(
        accessor.GetPluginInstance()->module()->pp_module(),
        identifiers[i]);
  }
  free(identifiers);
}

void SetPropertyDeprecated(PP_Var var,
                           PP_Var name,
                           PP_Var value,
                           PP_Var* exception) {
  ObjectAccessorWithIdentifierTryCatch accessor(var, name, exception);
  if (accessor.has_exception())
    return;

  NPVariant variant;
  if (!PPVarToNPVariantNoCopy(value, &variant)) {
    accessor.SetException(kInvalidValueException);
    return;
  }
  if (!WebBindings::setProperty(NULL, accessor.object()->np_object(),
                                accessor.identifier(), &variant))
    accessor.SetException(kUnableToSetPropertyException);
}

void DeletePropertyDeprecated(PP_Var var,
                              PP_Var name,
                              PP_Var* exception) {
  ObjectAccessorWithIdentifierTryCatch accessor(var, name, exception);
  if (accessor.has_exception())
    return;

  if (!WebBindings::removeProperty(NULL, accessor.object()->np_object(),
                                   accessor.identifier()))
    accessor.SetException(kUnableToRemovePropertyException);
}

PP_Var CallDeprecated(PP_Var var,
                      PP_Var method_name,
                      uint32_t argc,
                      PP_Var* argv,
                      PP_Var* exception) {
  ObjectAccessorTryCatch accessor(var, exception);
  if (accessor.has_exception())
    return PP_MakeUndefined();

  NPIdentifier identifier;
  if (method_name.type == PP_VARTYPE_UNDEFINED) {
    identifier = NULL;
  } else if (method_name.type == PP_VARTYPE_STRING) {
    // Specifically allow only string functions to be called.
    identifier = PPVarToNPIdentifier(method_name);
    if (!identifier) {
      accessor.SetException(kInvalidPropertyException);
      return PP_MakeUndefined();
    }
  } else {
    accessor.SetException(kInvalidPropertyException);
    return PP_MakeUndefined();
  }

  scoped_array<NPVariant> args;
  if (argc) {
    args.reset(new NPVariant[argc]);
    for (uint32_t i = 0; i < argc; ++i) {
      if (!PPVarToNPVariantNoCopy(argv[i], &args[i])) {
        // This argument was invalid, throw an exception & give up.
        accessor.SetException(kInvalidValueException);
        return PP_MakeUndefined();
      }
    }
  }

  bool ok;

  NPVariant result;
  if (identifier) {
    ok = WebBindings::invoke(NULL, accessor.object()->np_object(),
                             identifier, args.get(), argc, &result);
  } else {
    ok = WebBindings::invokeDefault(NULL, accessor.object()->np_object(),
                                    args.get(), argc, &result);
  }

  if (!ok) {
    // An exception may have been raised.
    accessor.SetException(kUnableToCallMethodException);
    return PP_MakeUndefined();
  }

  PP_Var ret = NPVariantToPPVar(accessor.GetPluginInstance(), &result);
  WebBindings::releaseVariantValue(&result);
  return ret;
}

PP_Var Construct(PP_Var var,
                 uint32_t argc,
                 PP_Var* argv,
                 PP_Var* exception) {
  ObjectAccessorTryCatch accessor(var, exception);
  if (accessor.has_exception())
    return PP_MakeUndefined();

  scoped_array<NPVariant> args;
  if (argc) {
    args.reset(new NPVariant[argc]);
    for (uint32_t i = 0; i < argc; ++i) {
      if (!PPVarToNPVariantNoCopy(argv[i], &args[i])) {
        // This argument was invalid, throw an exception & give up.
        accessor.SetException(kInvalidValueException);
        return PP_MakeUndefined();
      }
    }
  }

  NPVariant result;
  if (!WebBindings::construct(NULL, accessor.object()->np_object(),
                              args.get(), argc, &result)) {
    // An exception may have been raised.
    accessor.SetException(kUnableToConstructException);
    return PP_MakeUndefined();
  }

  PP_Var ret = NPVariantToPPVar(accessor.GetPluginInstance(), &result);
  WebBindings::releaseVariantValue(&result);
  return ret;
}

bool IsInstanceOfDeprecated(PP_Var var,
                            const PPP_Class_Deprecated* ppp_class,
                            void** ppp_class_data) {
  scoped_refptr<ObjectVar> object(ObjectVar::FromPPVar(var));
  if (!object)
    return false;  // Not an object at all.

  return PluginObject::IsInstanceOf(object->np_object(),
                                    ppp_class, ppp_class_data);
}

PP_Var CreateObjectDeprecated(PP_Instance instance_id,
                              const PPP_Class_Deprecated* ppp_class,
                              void* ppp_class_data) {
  PluginInstance* instance = ResourceTracker::Get()->GetInstance(instance_id);
  if (!instance) {
    DLOG(ERROR) << "Create object passed an invalid instance.";
    return PP_MakeNull();
  }
  return PluginObject::Create(instance, ppp_class, ppp_class_data);
}

PP_Var CreateObjectWithModuleDeprecated(PP_Module module_id,
                                        const PPP_Class_Deprecated* ppp_class,
                                        void* ppp_class_data) {
  PluginModule* module = ResourceTracker::Get()->GetModule(module_id);
  if (!module)
    return PP_MakeNull();
  return PluginObject::Create(module->GetSomeInstance(),
                              ppp_class, ppp_class_data);
}

const PPB_Var_Deprecated var_deprecated_interface = {
  &Var::PluginAddRefPPVar,
  &Var::PluginReleasePPVar,
  &VarFromUtf8,
  &VarToUtf8,
  &HasPropertyDeprecated,
  &HasMethodDeprecated,
  &GetProperty,
  &EnumerateProperties,
  &SetPropertyDeprecated,
  &DeletePropertyDeprecated,
  &CallDeprecated,
  &Construct,
  &IsInstanceOfDeprecated,
  &CreateObjectDeprecated,
  &CreateObjectWithModuleDeprecated,
};

const PPB_Var var_interface = {
  &Var::PluginAddRefPPVar,
  &Var::PluginReleasePPVar,
  &VarFromUtf8,
  &VarToUtf8
};

}  // namespace

// static
const PPB_Var* PPB_Var_Impl::GetVarInterface() {
  return &var_interface;
}

// static
const PPB_Var_Deprecated* PPB_Var_Impl::GetVarDeprecatedInterface() {
  return &var_deprecated_interface;
}

}  // namespace ppapi
}  // namespace webkit

