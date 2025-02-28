//===-- Variable.cpp --------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/Symbol/Variable.h"

#include "lldb/Core/Module.h"
#include "lldb/Core/Stream.h"
#include "lldb/Core/RegularExpression.h"
#include "lldb/Core/ValueObject.h"
#include "lldb/Core/ValueObjectVariable.h"
#include "lldb/Symbol/Block.h"
#include "lldb/Symbol/CompilerDecl.h"
#include "lldb/Symbol/CompilerDeclContext.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/SymbolFile.h"
#include "lldb/Symbol/Type.h"
#include "lldb/Symbol/TypeSystem.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Target/ABI.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/StackFrame.h"
#include "lldb/Target/Thread.h"
#include "lldb/Target/Target.h"

using namespace lldb;
using namespace lldb_private;

//----------------------------------------------------------------------
// Variable constructor
//----------------------------------------------------------------------
Variable::Variable (lldb::user_id_t uid,
                    const char *name,
                    const char *mangled,  // The mangled or fully qualified name of the variable.
                    const lldb::SymbolFileTypeSP &symfile_type_sp,
                    ValueType scope,
                    SymbolContextScope *context,
                    const RangeList& scope_range,
                    Declaration* decl_ptr,
                    const DWARFExpression& location,
                    bool external,
                    bool artificial,
                    bool static_member) :
    UserID(uid),
    m_name(name),
    m_mangled (ConstString(mangled)),
    m_symfile_type_sp(symfile_type_sp),
    m_scope(scope),
    m_owner_scope(context),
    m_scope_range(scope_range),
    m_declaration(decl_ptr),
    m_location(location),
    m_external(external),
    m_artificial(artificial),
    m_static_member(static_member)
{
}

//----------------------------------------------------------------------
// Destructor
//----------------------------------------------------------------------
Variable::~Variable()
{
}

lldb::LanguageType
Variable::GetLanguage () const
{
    SymbolContext variable_sc;
    m_owner_scope->CalculateSymbolContext(&variable_sc);
    if (variable_sc.comp_unit)
        return variable_sc.comp_unit->GetLanguage();
    return lldb::eLanguageTypeUnknown;
}



ConstString
Variable::GetName() const
{
    ConstString name = m_mangled.GetName(GetLanguage());
    if (name)
        return name;
    return m_name;
}

ConstString
Variable::GetUnqualifiedName() const
{
    return m_name;
}


bool
Variable::NameMatches (const ConstString &name) const
{
    if (m_name == name)
        return true;
    SymbolContext variable_sc;
    m_owner_scope->CalculateSymbolContext(&variable_sc);

    LanguageType language = eLanguageTypeUnknown;
    if (variable_sc.comp_unit)
        language = variable_sc.comp_unit->GetLanguage();
    return m_mangled.NameMatches (name, language);
}
bool
Variable::NameMatches (const RegularExpression& regex) const
{
    if (regex.Execute (m_name.AsCString()))
        return true;
    if (m_mangled)
        return m_mangled.NameMatches (regex, GetLanguage());
    return false;
}

Type *
Variable::GetType()
{
    if (m_symfile_type_sp)
        return m_symfile_type_sp->GetType();
    return nullptr;
}

void
Variable::Dump(Stream *s, bool show_context) const
{
    s->Printf("%p: ", static_cast<const void*>(this));
    s->Indent();
    *s << "Variable" << (const UserID&)*this;

    if (m_name)
        *s << ", name = \"" << m_name << "\"";

    if (m_symfile_type_sp)
    {
        Type *type = m_symfile_type_sp->GetType();
        if (type)
        {
            *s << ", type = {" << type->GetID() << "} " << (void*)type << " (";
            type->DumpTypeName(s);
            s->PutChar(')');
        }
    }

    if (m_scope != eValueTypeInvalid)
    {
        s->PutCString(", scope = ");
        switch (m_scope)
        {
        case eValueTypeVariableGlobal:       s->PutCString(m_external ? "global" : "static"); break;
        case eValueTypeVariableArgument:    s->PutCString("parameter"); break;
        case eValueTypeVariableLocal:        s->PutCString("local"); break;
        default:            *s << "??? (" << m_scope << ')';
        }
    }

    if (show_context && m_owner_scope != nullptr)
    {
        s->PutCString(", context = ( ");
        m_owner_scope->DumpSymbolContext(s);
        s->PutCString(" )");
    }

    bool show_fullpaths = false;
    m_declaration.Dump(s, show_fullpaths);

    if (m_location.IsValid())
    {
        s->PutCString(", location = ");
        lldb::addr_t loclist_base_addr = LLDB_INVALID_ADDRESS;
        if (m_location.IsLocationList())
        {
            SymbolContext variable_sc;
            m_owner_scope->CalculateSymbolContext(&variable_sc);
            if (variable_sc.function)
                loclist_base_addr = variable_sc.function->GetAddressRange().GetBaseAddress().GetFileAddress();
        }
        ABI *abi = nullptr;
        if (m_owner_scope)
        {
            ModuleSP module_sp (m_owner_scope->CalculateSymbolContextModule());
            if (module_sp)
                abi = ABI::FindPlugin (module_sp->GetArchitecture()).get();
        }
        m_location.GetDescription(s, lldb::eDescriptionLevelBrief, loclist_base_addr, abi);
    }

    if (m_external)
        s->PutCString(", external");

    if (m_artificial)
        s->PutCString(", artificial");

    s->EOL();
}

bool
Variable::DumpDeclaration (Stream *s, bool show_fullpaths, bool show_module)
{
    bool dumped_declaration_info = false;
    if (m_owner_scope)
    {
        SymbolContext sc;
        m_owner_scope->CalculateSymbolContext(&sc);
        sc.block = nullptr;
        sc.line_entry.Clear();
        bool show_inlined_frames = false;
        const bool show_function_arguments = true;
        const bool show_function_name = true;
    
        dumped_declaration_info = sc.DumpStopContext (s, 
                                                      nullptr,
                                                      Address(), 
                                                      show_fullpaths, 
                                                      show_module, 
                                                      show_inlined_frames,
                                                      show_function_arguments,
                                                      show_function_name);
        
        if (sc.function)
            s->PutChar(':');
    }
    if (m_declaration.DumpStopContext (s, false))
        dumped_declaration_info = true;
    return dumped_declaration_info;
}

size_t
Variable::MemorySize() const
{
    return sizeof(Variable);
}

CompilerDeclContext
Variable::GetDeclContext ()
{
    Type *type = GetType();
    if (type)
        return type->GetSymbolFile()->GetDeclContextContainingUID(GetID());
    return CompilerDeclContext();
}

CompilerDecl
Variable::GetDecl ()
{
    CompilerDecl decl;
    Type *type = GetType();
    if (type)
    {
        decl = type->GetSymbolFile()->GetDeclForUID(GetID());
        if (decl)
            decl.GetTypeSystem()->DeclLinkToObject(decl.GetOpaqueDecl(), shared_from_this());
    }
    return decl;
}

void
Variable::CalculateSymbolContext (SymbolContext *sc)
{
    if (m_owner_scope)
    {
        m_owner_scope->CalculateSymbolContext(sc);
        sc->variable = this;
    }
    else
        sc->Clear(false);
}

bool
Variable::LocationIsValidForFrame (StackFrame *frame)
{
    // Is the variable is described by a single location?
    if (!m_location.IsLocationList())
    {
        // Yes it is, the location is valid. 
        return true;
    }

    if (frame)
    {
        Function *function = frame->GetSymbolContext(eSymbolContextFunction).function;
        if (function)
        {
            TargetSP target_sp (frame->CalculateTarget());
            
            addr_t loclist_base_load_addr = function->GetAddressRange().GetBaseAddress().GetLoadAddress (target_sp.get());
            if (loclist_base_load_addr == LLDB_INVALID_ADDRESS)
                return false;
            // It is a location list. We just need to tell if the location
            // list contains the current address when converted to a load
            // address
            return m_location.LocationListContainsAddress (loclist_base_load_addr, 
                                                           frame->GetFrameCodeAddress().GetLoadAddress (target_sp.get()));
        }
    }
    return false;
}

bool
Variable::LocationIsValidForAddress (const Address &address)
{
    // Be sure to resolve the address to section offset prior to 
    // calling this function.
    if (address.IsSectionOffset())
    {
        SymbolContext sc;
        CalculateSymbolContext(&sc);
        if (sc.module_sp == address.GetModule())
        {
            // Is the variable is described by a single location?
            if (!m_location.IsLocationList())
            {
                // Yes it is, the location is valid. 
                return true;
            }
            
            if (sc.function)
            {
                addr_t loclist_base_file_addr = sc.function->GetAddressRange().GetBaseAddress().GetFileAddress();
                if (loclist_base_file_addr == LLDB_INVALID_ADDRESS)
                    return false;
                // It is a location list. We just need to tell if the location
                // list contains the current address when converted to a load
                // address
                return m_location.LocationListContainsAddress (loclist_base_file_addr, 
                                                               address.GetFileAddress());
            }
        }
    }
    return false;
}

bool
Variable::IsInScope (StackFrame *frame)
{
    switch (m_scope)
    {
    case eValueTypeRegister:
    case eValueTypeRegisterSet:
        return frame != nullptr;

    case eValueTypeConstResult:
    case eValueTypeVariableGlobal:
    case eValueTypeVariableStatic:
        return true;

    case eValueTypeVariableArgument:
    case eValueTypeVariableLocal:
        if (frame)
        {
            // We don't have a location list, we just need to see if the block
            // that this variable was defined in is currently
            Block *deepest_frame_block = frame->GetSymbolContext(eSymbolContextBlock).block;
            if (deepest_frame_block)
            {
                SymbolContext variable_sc;
                CalculateSymbolContext (&variable_sc);

                // Check for static or global variable defined at the compile unit 
                // level that wasn't defined in a block
                if (variable_sc.block == nullptr)
                    return true;

                // Check if the variable is valid in the current block
                if (variable_sc.block != deepest_frame_block &&
                    !variable_sc.block->Contains(deepest_frame_block))
                    return false;

                // If no scope range is specified then it means that the scope is the same as the
                // scope of the enclosing lexical block.
                if (m_scope_range.IsEmpty())
                    return true;

                addr_t file_address = frame->GetFrameCodeAddress().GetFileAddress();
                return m_scope_range.FindEntryThatContains(file_address) != nullptr;
            }
        }
        break;

    default:
        break;
    }
    return false;
}

Error
Variable::GetValuesForVariableExpressionPath (const char *variable_expr_path,
                                              ExecutionContextScope *scope,
                                              GetVariableCallback callback,
                                              void *baton,
                                              VariableList &variable_list,
                                              ValueObjectList &valobj_list)
{
    Error error;
    if (variable_expr_path && callback)
    {
        switch (variable_expr_path[0])
        {
        case '*':
            {
                error = Variable::GetValuesForVariableExpressionPath (variable_expr_path + 1,
                                                                      scope,
                                                                      callback,
                                                                      baton,
                                                                      variable_list,
                                                                      valobj_list);
                if (error.Success())
                {
                    for (uint32_t i=0; i<valobj_list.GetSize(); )
                    {
                        Error tmp_error;
                        ValueObjectSP valobj_sp (valobj_list.GetValueObjectAtIndex(i)->Dereference(tmp_error));
                        if (tmp_error.Fail())
                        {
                            variable_list.RemoveVariableAtIndex (i);
                            valobj_list.RemoveValueObjectAtIndex (i);
                        }
                        else
                        {
                            valobj_list.SetValueObjectAtIndex (i, valobj_sp);
                            ++i;
                        }
                    }
                }
                else
                {
                    error.SetErrorString ("unknown error");
                }
                return error;
            }
            break;
        
        case '&':
            {
                error = Variable::GetValuesForVariableExpressionPath (variable_expr_path + 1,
                                                                      scope,
                                                                      callback,
                                                                      baton,
                                                                      variable_list,
                                                                      valobj_list);
                if (error.Success())
                {
                    for (uint32_t i=0; i<valobj_list.GetSize(); )
                    {
                        Error tmp_error;
                        ValueObjectSP valobj_sp (valobj_list.GetValueObjectAtIndex(i)->AddressOf(tmp_error));
                        if (tmp_error.Fail())
                        {
                            variable_list.RemoveVariableAtIndex (i);
                            valobj_list.RemoveValueObjectAtIndex (i);
                        }
                        else
                        {
                            valobj_list.SetValueObjectAtIndex (i, valobj_sp);
                            ++i;
                        }
                    }
                }
                else
                {
                    error.SetErrorString ("unknown error");
                }
                return error;
            }
            break;
            
        default:
            {
                static RegularExpression g_regex ("^([A-Za-z_:][A-Za-z_0-9:]*)(.*)");
                RegularExpression::Match regex_match(1);
                if (g_regex.Execute(variable_expr_path, &regex_match))
                {
                    std::string variable_name;
                    if (regex_match.GetMatchAtIndex(variable_expr_path, 1, variable_name))
                    {
                        variable_list.Clear();
                        if (callback (baton, variable_name.c_str(), variable_list))
                        {
                            uint32_t i=0;
                            while (i < variable_list.GetSize())
                            {
                                VariableSP var_sp (variable_list.GetVariableAtIndex (i));
                                ValueObjectSP valobj_sp;
                                if (var_sp)
                                {
                                    ValueObjectSP variable_valobj_sp(ValueObjectVariable::Create (scope, var_sp));
                                    if (variable_valobj_sp)
                                    {
                                        const char *variable_sub_expr_path = variable_expr_path + variable_name.size();
                                        if (*variable_sub_expr_path)
                                        {
                                            const char* first_unparsed = nullptr;
                                            ValueObject::ExpressionPathScanEndReason reason_to_stop;
                                            ValueObject::ExpressionPathEndResultType final_value_type;
                                            ValueObject::GetValueForExpressionPathOptions options;
                                            ValueObject::ExpressionPathAftermath final_task_on_target;

                                            valobj_sp = variable_valobj_sp->GetValueForExpressionPath (variable_sub_expr_path,
                                                                                                       &first_unparsed,
                                                                                                       &reason_to_stop,
                                                                                                       &final_value_type,
                                                                                                       options,
                                                                                                       &final_task_on_target);
                                            if (!valobj_sp)
                                            {
                                                error.SetErrorStringWithFormat ("invalid expression path '%s' for variable '%s'",
                                                                                variable_sub_expr_path,
                                                                                var_sp->GetName().GetCString());
                                            }
                                        }
                                        else
                                        {
                                            // Just the name of a variable with no extras
                                            valobj_sp = variable_valobj_sp;
                                        }
                                    }
                                }

                                if (!var_sp || !valobj_sp)
                                {
                                    variable_list.RemoveVariableAtIndex (i);
                                }
                                else
                                {
                                    valobj_list.Append(valobj_sp);
                                    ++i;
                                }
                            }
                            
                            if (variable_list.GetSize() > 0)
                            {
                                error.Clear();
                                return error;
                            }
                        }
                    }
                }
                error.SetErrorStringWithFormat ("unable to extract a variable name from '%s'", variable_expr_path);
            }
            break;
        }
    }
    error.SetErrorString ("unknown error");
    return error;
}

bool
Variable::DumpLocationForAddress (Stream *s, const Address &address)
{
    // Be sure to resolve the address to section offset prior to 
    // calling this function.
    if (address.IsSectionOffset())
    {
        SymbolContext sc;
        CalculateSymbolContext(&sc);
        if (sc.module_sp == address.GetModule())
        {
            ABI *abi = nullptr;
            if (m_owner_scope)
            {
                ModuleSP module_sp (m_owner_scope->CalculateSymbolContextModule());
                if (module_sp)
                    abi = ABI::FindPlugin (module_sp->GetArchitecture()).get();
            }

            const addr_t file_addr = address.GetFileAddress();
            if (sc.function)
            {
                if (sc.function->GetAddressRange().ContainsFileAddress(address))
                {
                    addr_t loclist_base_file_addr = sc.function->GetAddressRange().GetBaseAddress().GetFileAddress();
                    if (loclist_base_file_addr == LLDB_INVALID_ADDRESS)
                        return false;
                    return m_location.DumpLocationForAddress (s, 
                                                              eDescriptionLevelBrief, 
                                                              loclist_base_file_addr, 
                                                              file_addr,
                                                              abi);
                }
            }
            return m_location.DumpLocationForAddress (s, 
                                                      eDescriptionLevelBrief, 
                                                      LLDB_INVALID_ADDRESS, 
                                                      file_addr,
                                                      abi);
        }
    }
    return false;
}


static void
PrivateAutoComplete (StackFrame *frame,
                     const std::string &partial_path,
                     const std::string &prefix_path, // Anything that has been resolved already will be in here
                     const CompilerType& compiler_type,
                     StringList &matches,
                     bool &word_complete);

static void
PrivateAutoCompleteMembers (StackFrame *frame,
                            const std::string &partial_member_name,
                            const std::string &partial_path,
                            const std::string &prefix_path, // Anything that has been resolved already will be in here
                            const CompilerType& compiler_type,
                            StringList &matches,
                            bool &word_complete);

static void
PrivateAutoCompleteMembers (StackFrame *frame,
                            const std::string &partial_member_name,
                            const std::string &partial_path,
                            const std::string &prefix_path, // Anything that has been resolved already will be in here
                            const CompilerType& compiler_type,
                            StringList &matches,
                            bool &word_complete)
{

    // We are in a type parsing child members
    const uint32_t num_bases = compiler_type.GetNumDirectBaseClasses();
    
    if (num_bases > 0)
    {
        for (uint32_t i = 0; i < num_bases; ++i)
        {
            CompilerType base_class_type = compiler_type.GetDirectBaseClassAtIndex(i, nullptr);
            
            PrivateAutoCompleteMembers (frame,
                                        partial_member_name,
                                        partial_path,
                                        prefix_path,
                                        base_class_type.GetCanonicalType(),
                                        matches,
                                        word_complete);
        }
    }

    const uint32_t num_vbases = compiler_type.GetNumVirtualBaseClasses();
    
    if (num_vbases > 0)
    {
        for (uint32_t i = 0; i < num_vbases; ++i)
        {
            CompilerType vbase_class_type = compiler_type.GetVirtualBaseClassAtIndex(i,nullptr);
            
            PrivateAutoCompleteMembers (frame,
                                        partial_member_name,
                                        partial_path,
                                        prefix_path,
                                        vbase_class_type.GetCanonicalType(),
                                        matches,
                                        word_complete);
        }
    }

    // We are in a type parsing child members
    const uint32_t num_fields = compiler_type.GetNumFields();
    
    if (num_fields > 0)
    {
        for (uint32_t i = 0; i < num_fields; ++i)
        {
            std::string member_name;
            
            CompilerType member_compiler_type = compiler_type.GetFieldAtIndex (i, member_name, nullptr, nullptr, nullptr);
            
            if (partial_member_name.empty() ||
                member_name.find(partial_member_name) == 0)
            {
                if (member_name == partial_member_name)
                {
                    PrivateAutoComplete (frame,
                                         partial_path,
                                         prefix_path + member_name, // Anything that has been resolved already will be in here
                                         member_compiler_type.GetCanonicalType(),
                                         matches,
                                         word_complete);
                }
                else
                {
                    matches.AppendString (prefix_path + member_name);
                }
            }
        }
    }
}

static void
PrivateAutoComplete (StackFrame *frame,
                     const std::string &partial_path,
                     const std::string &prefix_path, // Anything that has been resolved already will be in here
                     const CompilerType& compiler_type,
                     StringList &matches,
                     bool &word_complete)
{
//    printf ("\nPrivateAutoComplete()\n\tprefix_path = '%s'\n\tpartial_path = '%s'\n", prefix_path.c_str(), partial_path.c_str());
    std::string remaining_partial_path;

    const lldb::TypeClass type_class = compiler_type.GetTypeClass();
    if (partial_path.empty())
    {
        if (compiler_type.IsValid())
        {
            switch (type_class)
            {
                default:
                case eTypeClassArray:
                case eTypeClassBlockPointer:
                case eTypeClassBuiltin:
                case eTypeClassComplexFloat:
                case eTypeClassComplexInteger:
                case eTypeClassEnumeration:
                case eTypeClassFunction:
                case eTypeClassMemberPointer:
                case eTypeClassReference:
                case eTypeClassTypedef:
                case eTypeClassVector:
                    {
                        matches.AppendString (prefix_path);
                        word_complete = matches.GetSize() == 1;
                    }
                    break;
                    
                case eTypeClassClass:
                case eTypeClassStruct:
                case eTypeClassUnion:
                    if (prefix_path.back() != '.')
                        matches.AppendString (prefix_path + '.');
                    break;

                case eTypeClassObjCObject:
                case eTypeClassObjCInterface:
                    break;
                case eTypeClassObjCObjectPointer:
                case eTypeClassPointer:
                    {
                        bool omit_empty_base_classes = true;
                        if (compiler_type.GetNumChildren (omit_empty_base_classes) > 0)
                            matches.AppendString (prefix_path + "->");
                        else
                        {
                            matches.AppendString (prefix_path);
                            word_complete = true;
                        }
                    }
                    break;
            }
        }
        else
        {
            if (frame)
            {
                const bool get_file_globals = true;
                
                VariableList *variable_list = frame->GetVariableList(get_file_globals);
                
                if (variable_list)
                {
                    const size_t num_variables = variable_list->GetSize();
                    for (size_t i=0; i<num_variables; ++i)
                    {
                        Variable *variable = variable_list->GetVariableAtIndex(i).get();
                        matches.AppendString (variable->GetName().AsCString());
                    }
                }
            }
        }
    }
    else
    {
        const char ch = partial_path[0];
        switch (ch)
        {
        case '*':
            if (prefix_path.empty())
            {
                PrivateAutoComplete (frame,
                                     partial_path.substr(1),
                                     std::string("*"),
                                     compiler_type,
                                     matches,
                                     word_complete);
            }
            break;

        case '&':
            if (prefix_path.empty())
            {
                PrivateAutoComplete (frame,
                                     partial_path.substr(1),
                                     std::string("&"),
                                     compiler_type,
                                     matches,
                                     word_complete);
            }
            break;

        case '-':
            if (partial_path[1] == '>' && !prefix_path.empty())
            {
                switch (type_class)
                {
                    case lldb::eTypeClassPointer:
                        {
                            CompilerType pointee_type(compiler_type.GetPointeeType());
                            if (partial_path[2])
                            {
                                // If there is more after the "->", then search deeper
                                PrivateAutoComplete (frame,
                                                     partial_path.substr(2),
                                                     prefix_path + "->",
                                                     pointee_type.GetCanonicalType(),
                                                     matches,
                                                     word_complete);
                            }
                            else
                            {
                                // Nothing after the "->", so list all members
                                PrivateAutoCompleteMembers (frame,
                                                            std::string(),
                                                            std::string(),
                                                            prefix_path + "->",
                                                            pointee_type.GetCanonicalType(),
                                                            matches,
                                                            word_complete);                            
                            }
                        }
                        break;
                    default:
                        break;
                }
            }
            break;
            
        case '.':
            if (compiler_type.IsValid())
            {
                switch (type_class)
                {
                    case lldb::eTypeClassUnion:
                    case lldb::eTypeClassStruct:
                    case lldb::eTypeClassClass:
                        if (partial_path[1])
                        {
                            // If there is more after the ".", then search deeper
                            PrivateAutoComplete (frame,
                                                 partial_path.substr(1),
                                                 prefix_path + ".",
                                                 compiler_type,
                                                 matches,
                                                 word_complete);
                            
                        }
                        else
                        {
                            // Nothing after the ".", so list all members
                            PrivateAutoCompleteMembers (frame,
                                                        std::string(),
                                                        partial_path,
                                                        prefix_path + ".",
                                                        compiler_type,
                                                        matches,
                                                        word_complete);
                        }
                        break;
                    default:
                        break;
                }
            }
            break;
        default:
            if (isalpha(ch) || ch == '_' || ch == '$')
            {
                const size_t partial_path_len = partial_path.size();
                size_t pos = 1;
                while (pos < partial_path_len)
                {
                    const char curr_ch = partial_path[pos];
                    if (isalnum(curr_ch) || curr_ch == '_'  || curr_ch == '$')
                    {
                        ++pos;
                        continue;
                    }
                    break;
                }

                std::string token(partial_path, 0, pos);
                remaining_partial_path = partial_path.substr(pos);
                
                if (compiler_type.IsValid())
                {
                    PrivateAutoCompleteMembers (frame,
                                                token,
                                                remaining_partial_path,
                                                prefix_path,
                                                compiler_type,
                                                matches,
                                                word_complete);
                }
                else if (frame)
                {
                    // We haven't found our variable yet
                    const bool get_file_globals = true;
                    
                    VariableList *variable_list = frame->GetVariableList(get_file_globals);
                    
                    if (!variable_list)
                        break;
                    
                    const size_t num_variables = variable_list->GetSize();
                    for (size_t i=0; i<num_variables; ++i)
                    {
                        Variable *variable = variable_list->GetVariableAtIndex(i).get();
                        
                        if (!variable)
                            continue;
                        
                        const char *variable_name = variable->GetName().AsCString();
                        if (strstr(variable_name, token.c_str()) == variable_name)
                        {
                            if (strcmp (variable_name, token.c_str()) == 0)
                            {
                                Type *variable_type = variable->GetType();
                                if (variable_type)
                                {
                                    CompilerType variable_compiler_type (variable_type->GetForwardCompilerType ());
                                    PrivateAutoComplete (frame,
                                                         remaining_partial_path,
                                                         prefix_path + token, // Anything that has been resolved already will be in here
                                                         variable_compiler_type.GetCanonicalType(),
                                                         matches,
                                                         word_complete);
                                }
                                else
                                {
                                    matches.AppendString (prefix_path + variable_name);
                                }
                            }
                            else if (remaining_partial_path.empty())
                            {
                                matches.AppendString (prefix_path + variable_name);
                            }
                        }
                    }
                }
            }
            break;
        }
    }
}



size_t
Variable::AutoComplete (const ExecutionContext &exe_ctx,
                        const char *partial_path_cstr,
                        StringList &matches,
                        bool &word_complete)
{
    word_complete = false;
    std::string partial_path;
    std::string prefix_path;
    CompilerType compiler_type;
    if (partial_path_cstr && partial_path_cstr[0])
        partial_path = partial_path_cstr;

    PrivateAutoComplete (exe_ctx.GetFramePtr(),
                         partial_path,
                         prefix_path,
                         compiler_type,
                         matches,
                         word_complete);

    return matches.GetSize();
}

