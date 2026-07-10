module Volt::Frontend


  # Phase B collection pass: declares every nominal type (`class`/`struct`/
  # `mixin`/`module`) up front so forward references resolve, then resolves
  # each one : fields, superclass, mixins, methods : in dependency order.
  #
  # Field-type resolution recurses through `resolve` (cycle-checked via
  # `@resolving`) because a struct embedded by value needs its own layout
  # packed before it can size the field that holds it. Method parameter/return
  # types do *not* recurse : a method may reference its own enclosing type
  # (`def dot(other : Vec3)` inside `Vec3`) without that being a real cycle,
  # since methods don't participate in sizing. By the time `collect` returns
  # every declared type has been resolved at least once, so those references
  # end up correctly populated regardless of visitation order (nominals are
  # shared, mutable instances).
  class TypeCollector
    getter types         : Hash( String, TypeInfo )
    getter nominals      : Hash( String, NominalType )
    getter methods       : Array( FuncDecl )
    getter method_entries : Array( { FuncDecl, String, FuncSig } )
    getter decls         : Hash( String, ANode )
    getter resolved      : Set( String )

    def initialize( @bag : DiagnosticBag )
      @decls     = {} of String => ANode
      @types     = {} of String => TypeInfo
      @nominals  = {} of String => NominalType
      @methods   = [] of FuncDecl
      @method_entries = [] of { FuncDecl, String, FuncSig }
      @resolving = Set( String ).new
      @resolved  = Set( String ).new
    end

    def collect( nodes : Array( ANode ) ) : Nil
      nodes.each { |node| declare( node ) }
      @decls.each_key { |name| resolve( name ) }
    end

    # -----------------------------------------------------------------------------------

    # `prefix` is the enclosing module's fully-qualified name, threaded through
    # so a type declared inside `module SmartCity` is registered under *both*
    # its bare canonical name (`WaterSensor`, used for unqualified references
    # inside the module and for method mangling) and a qualified alias
    # (`SmartCity::WaterSensor`, used at call sites outside the module). Both
    # keys point at the same `TypeInfo`/`NominalType` instance — an MVP flatten
    # that assumes no bare-name collisions across modules.
    private def declare( node : ANode, prefix : String? = nil ) : Nil
      name =
        case node
        when ClassDecl  then node.name
        when StructDecl then node.name
        when MixinDecl  then node.name
        when ModuleDecl then node.name
        else                 return
        end

      # Reopening a primitive (`struct Int … end` in the Core, possibly
      # again in user code) attaches methods to a builtin : no `NominalType`
      # is registered — the name keeps resolving to the primitive in
      # annotations — only a `TypeInfo` carrying the method table. Unlike a
      # real class/struct, a primitive reopening is additive "monkey
      # patching": a second `struct Int … end` elsewhere merges its methods
      # into the same `TypeInfo` rather than duplicate-erroring.
      # `resolve` routes it to `resolve_primitive_reopen`.
      if node.is_a?( StructDecl ) && Type.from_primitive_name( name )
        if existing = @decls[ name ]?
          existing.as( StructDecl ).body.concat( node.body )
          return
        end
        @decls[ name ] = node
        @types[ name ] = TypeInfo.new( NominalKind::Struct, name, @decls.size - 1 )
        return
      end

      if @decls.has_key?( name )
        @bag << Catalog::Sema.duplicate_definition( name, node.loc, @decls[ name ].loc )
        return
      end

      kind =
        case node
        when ClassDecl  then NominalKind::Class
        when StructDecl then NominalKind::Struct
        when MixinDecl  then NominalKind::Mixin
        else                 NominalKind::Module
        end

      nominal_kind = kind.struct? ? TypeKind::Struct : TypeKind::Object
      type_id      = @decls.size

      nom  = NominalType.new( name, nominal_kind, type_id )
      info = TypeInfo.new( kind, name, type_id )

      @decls[ name ]    = node
      @nominals[ name ] = nom
      @types[ name ]    = info

      if prefix
        qualified = "#{prefix}::#{name}"
        @nominals[ qualified ] = nom
        @types[ qualified ]    = info
      end

      # A module is a namespace: flatten its nested type declarations into the
      # same (aliased) collection so they resolve as first-class types.
      if node.is_a?( ModuleDecl )
        child_prefix = prefix ? "#{prefix}::#{name}" : name
        node.body.each { |child| declare( child, child_prefix ) }
      end
    end

    private def resolve( name : String ) : TypeInfo?
      return @types[ name ]? if @resolved.includes?( name )
      node = @decls[ name ]?
      return nil unless node

      if @resolving.includes?( name )
        @bag << Catalog::Sema.circular_type( name, node.loc )
        @resolved << name
        return @types[ name ]?
      end

      @resolving << name
      info = @types[ name ]

      case node
      when ClassDecl  then resolve_class( node, info )
      when StructDecl then resolve_struct( node, info )
      when MixinDecl  then resolve_mixin( node, info )
      when ModuleDecl then resolve_module( node, info )
      end

      @resolving.delete( name )
      @resolved << name
      info
    end

    private def resolve_class( node : ClassDecl, info : TypeInfo ) : Nil
      info.is_abstract = node.is_abstract

      base_layout = nil
      sup_info    = nil
      if sup = node.superclass
        sup_decl = @decls[ sup ]?
        if sup_decl.is_a?( ClassDecl )
          sup_info   = resolve( sup )
          info.superclass = sup
          base_layout = sup_info.try( &.layout )
        else
          @bag << Catalog::Sema.unknown_superclass( info.name, sup, node.loc )
        end
      end

      mixin_names = resolve_mixins( info.name, node.mixins, node.loc )
      info.mixins = mixin_names
      # Force-resolve every mixin now (unlike the superclass, this can't
      # cycle back into a layout computation) so its `methods_ast` is
      # populated in time for `build_vtable` below, regardless of whether
      # this class or the mixin happens to come first in declaration order.
      mixin_infos = mixin_names.compact_map { |m| resolve( m ) }

      base_reg_layout = node.superclass.try { |s| @nominals[ s ]?.try( &.reg_layout ) }
      fields          = collect_fields( node.body, base_layout )
      byte_layout     = TypeLayout.pack( fields, base_layout )
      set_layout( info, byte_layout, build_reg_layout( byte_layout, base_reg_layout ) )

      collect_methods( node.body, info )
      build_vtable( info, sup_info, mixin_infos )
      check_abstract_completeness( info, node.loc )
    end

    # Assigns each dispatchable method name a stable slot index : starts from
    # the superclass's own table verbatim (an override reuses its inherited
    # index — this is what lets a `Device`-typed call site read the right
    # slot on a `UsbDrive` instance), then appends each mixin's new names in
    # inclusion order, then any further name the class declares itself.
    # `initialize`/`finalize` never dispatch virtually, so they're excluded.
    private def build_vtable( info : TypeInfo, base : TypeInfo?, mixins : Array( TypeInfo ) ) : Nil
      layout   = base.try( &.vtable_layout.dup ) || {} of String => Int32
      next_idx = base.try( &.vtable_size ) || 0

      mixins.each do |m|
        m.methods_ast.each_key do |name|
          next if layout.has_key?( name )
          layout[ name ] = next_idx
          next_idx += 1
        end
      end

      info.methods_ast.each_key do |name|
        next if name == "initialize" || name == "finalize" || name.starts_with?( "initialize/" )
        next if layout.has_key?( name )
        layout[ name ] = next_idx
        next_idx += 1
      end

      info.vtable_layout = layout
      info.vtable_size   = next_idx
    end

    private def resolve_struct( node : StructDecl, info : TypeInfo ) : Nil
      return resolve_primitive_reopen( node, info ) if Type.from_primitive_name( node.name )

      fields      = collect_fields( node.body, nil )
      byte_layout = TypeLayout.pack( fields )
      set_layout( info, byte_layout, build_reg_layout( byte_layout, nil ) )
      collect_methods( node.body, info )
    end

    # A primitive reopening carries methods only : a builtin's representation
    # is fixed (one register slot, no heap layout), so fields and the
    # construction/destruction lifecycle are meaningless on it.
    private def resolve_primitive_reopen( node : StructDecl, info : TypeInfo ) : Nil
      node.body.each do |n|
        case n
        when FieldDecl
          @bag << Catalog::Sema.unsupported_expr( "field declaration in primitive type `#{info.name}`", n.loc )
        when FuncDecl
          if n.name == "initialize" || n.name == "finalize"
            @bag << Catalog::Sema.unsupported_expr( "`#{n.name}` in primitive type `#{info.name}`", n.loc )
          end
        end
      end
      collect_methods( node.body, info )
    end

    private def resolve_mixin( node : MixinDecl, info : TypeInfo ) : Nil
      collect_methods( node.body, info )
    end

    private def resolve_module( node : ModuleDecl, info : TypeInfo ) : Nil
      info.extend_self = node.body.any?( ExtendSelfDecl )
      collect_class_vars( node.body, info )
      collect_methods( node.body, info )
    end

    # `@@name : Type [= default]` declarations become module-global variables.
    private def collect_class_vars( body : Array( ANode ), info : TypeInfo ) : Nil
      body.each do |node|
        next unless node.is_a?( ClassVarDecl )
        if info.class_vars.has_key?( node.name )
          @bag << Catalog::Sema.duplicate_definition( "@@#{node.name}", node.loc, nil )
          next
        end
        info.class_vars[ node.name ] = resolve_method_type( node.type_ann )
      end
    end

    private def set_layout( info : TypeInfo, layout : TypeLayout, reg_layout : TypeLayout ) : Nil
      info.layout     = layout
      info.reg_layout = reg_layout
      nom             = @nominals[ info.name ]
      nom.layout      = layout
      nom.reg_layout  = reg_layout
    end

    # Derives the Tier-0 register-slot layout from the already-packed byte
    # layout: every field becomes 1 slot, except a nested struct field which
    # flattens in as N slots (N = that struct's own slot count) : no
    # alignment/padding at this granularity (every slot is a uniform `Value`
    # cell). Reuses the same `TypeLayout.pack` used for byte packing, just fed
    # slot-sized inputs, so inheritance-prefix sharing falls out for free.
    private def build_reg_layout( byte_layout : TypeLayout, base_reg_layout : TypeLayout? ) : TypeLayout
      base_count = base_reg_layout.try( &.fields.size ) || 0
      own_fields = byte_layout.fields[ base_count.. ]
      reg_slots  = own_fields.map do |f|
        slot_size =
          if ( t = f.type ).is_a?( NominalType ) && t.kind.struct? && ( rl = t.reg_layout )
            rl.total_size
          else
            1
          end
        TypeLayout.slot( f.name, f.type, size: slot_size, align: 1, is_ref: f.is_ref )
      end
      TypeLayout.pack( reg_slots, base_reg_layout )
    end

    private def resolve_mixins( owner : String, names : Array( String ), loc : Span ) : Array( String )
      names.compact_map do |m|
        case @decls[ m ]?
        when MixinDecl
          m
        when ModuleDecl
          @bag << Catalog::Sema.include_module_forbidden( m, loc )
          nil
        when Nil
          @bag << Catalog::Sema.unknown_mixin( owner, m, loc )
          nil
        else
          @bag << Catalog::Sema.unknown_mixin( owner, m, loc )
          nil
        end
      end
    end

    # -----------------------------------------------------------------------------------

    private def collect_fields( body : Array( ANode ), base_layout : TypeLayout? ) : Array( FieldSlot )
      slots = [] of FieldSlot
      seen  = Set( String ).new
      base_layout.try { |bl| bl.fields.each { |f| seen << f.name } }

      body.each do |node|
        next unless node.is_a?( FieldDecl )
        if seen.includes?( node.name )
          @bag << Catalog::Sema.duplicate_definition( node.name, node.loc, nil )
          next
        end
        slots << TypeLayout.slot( node.name, resolve_field_type( node.type_ann ) )
        seen << node.name
      end

      # Style-1 constructor shorthand (`def initialize( @x : T )`) implicitly
      # declares any field not already covered by an explicit `FieldDecl` or
      # inherited from the superclass.
      inits = body.select { |n| n.is_a?( FuncDecl ) && n.name == "initialize" }.map( &.as( FuncDecl ) )
      inits.each do |fn|
        fn.params.each do |p|
          next unless p.is_ivar
          next if seen.includes?( p.name )
          ty =
            if ann = p.type_ann
              resolve_field_type( ann )
            else
              @bag << Catalog::Sema.param_needs_type( p.name, "initialize", p.loc )
              Type::UNKNOWN
            end
          slots << TypeLayout.slot( p.name, ty )
          seen << p.name
        end
      end

      slots
    end

    # Field types affect layout sizing, so a locally-declared type referenced
    # here must be fully resolved first (recursive, cycle-checked) — but *only*
    # a `struct` field, which embeds its target by value and so needs that
    # target's layout packed before it can be sized. A `class`-typed field
    # holds a reference (always pointer-sized, independent of the target's
    # layout), so forcing its resolution here would misfire the cycle guard on
    # legitimately self-referential data structures (linked lists, trees). Such
    # a class field is left for the outer `collect` loop to resolve; deferring
    # it is safe because `Type.from_annotation` only needs the `NominalType`
    # instance (created eagerly in `declare`), not its layout.
    private def resolve_field_type( ann : ATypeNode ) : Type
      if ann.is_a?( SimpleType ) && @decls[ ann.name ]?.is_a?( StructDecl )
        resolve( ann.name )
      end
      Type.from_annotation( ann, @nominals ) || begin
        @bag << Catalog::Sema.unsupported_annotation( ann.loc )
        Type::UNKNOWN
      end
    end

    # Method signatures never need to force-resolve a type : every declared
    # type gets resolved by the end of `collect` regardless, and forcing here
    # would misfire the cycle guard on ordinary self-referencing signatures.
    private def resolve_method_type( ann : ATypeNode ) : Type
      Type.from_annotation( ann, @nominals ) || begin
        @bag << Catalog::Sema.unsupported_annotation( ann.loc )
        Type::UNKNOWN
      end
    end

    private def collect_methods( body : Array( ANode ), info : TypeInfo ) : Nil
      # `initialize` is the one name allowed to overload — resolved by arity.
      # When several overloads exist, each is keyed `initialize/<arity>` so
      # every overload compiles to its own chunk; a single `initialize` keeps
      # the plain key (the shape every existing lookup expects).
      init_count = body.count { |n| n.is_a?( FuncDecl ) && n.name == "initialize" }

      body.each do |node|
        next unless node.is_a?( FuncDecl )
        sig = build_method_sig( node, info )
        key = node.name
        case node.name
        when "initialize"
          arity = node.params.size
          if first = info.initializers[ arity ]?
            @bag << Catalog::Sema.duplicate_definition( "initialize", node.loc, first.decl_span )
            next
          end
          info.initializers[ arity ] = sig
          info.initializer ||= sig
          key = "initialize/#{arity}" if init_count > 1
        when "finalize"
          unless node.params.empty?
            @bag << Catalog::Sema.finalize_has_arguments( node.loc )
            next
          end
          if first = info.finalizer
            @bag << Catalog::Sema.duplicate_definition( "finalize", node.loc, first.decl_span )
            next
          end
          info.finalizer = sig
        end
        info.methods[ key ]     = sig
        info.methods_ast[ key ] = node
        @methods << node
        @method_entries << { node, info.name, sig }
      end
    end

    private def build_method_sig( decl : FuncDecl, info : TypeInfo ) : FuncSig
      params = decl.params.map do |p|
        if p.is_ivar
          if field = info.layout.try( &.field?( p.name ) )
            field.type
          else
            @bag << Catalog::Sema.unknown_instance_var( info.name, p.name, p.loc )
            Type::UNKNOWN
          end
        elsif ann = p.type_ann
          resolve_method_type( ann )
        else
          @bag << Catalog::Sema.param_needs_type( p.name, decl.name, p.loc )
          Type::UNKNOWN
        end
      end

      ret =
        if rt = decl.return_type
          resolve_method_type( rt )
        elsif decl.name == "initialize" || decl.name == "finalize"
          Type::NIL
        else
          Type::UNKNOWN
        end

      is_static = info.kind.module? ? ( decl.is_static || info.extend_self ) : decl.is_static

      FuncSig.new( decl.name, params, ret, decl_span: decl.loc, owner: info.name,
                    is_static: is_static, visibility: decl.visibility )
    end

    # A concrete class must provide a non-abstract override for every abstract
    # method declared anywhere in its superclass chain.
    private def check_abstract_completeness( info : TypeInfo, loc : Span ) : Nil
      return if info.is_abstract

      chain = [] of TypeInfo
      cur   = info.superclass
      while cur
        parent = @types[ cur ]?
        break unless parent
        chain << parent
        cur = parent.superclass
      end

      chain.each do |ancestor|
        ancestor_decl = @decls[ ancestor.name ]?
        next unless ancestor_decl.is_a?( ClassDecl )
        ancestor_decl.body.each do |node|
          next unless node.is_a?( FuncDecl ) && node.is_abstract
          implemented = provides_concrete?( info, node.name ) ||
                        chain.any? { |a| a.name != ancestor.name && provides_concrete?( a, node.name ) }
          unless implemented
            @bag << Catalog::Sema.missing_abstract_impl( info.name, node.name, ancestor.name, loc )
          end
        end
      end
    end

    private def provides_concrete?( info : TypeInfo, method : String ) : Bool
      decl = @decls[ info.name ]?
      return false unless decl.is_a?( ClassDecl )
      decl.body.any? { |n| n.is_a?( FuncDecl ) && n.name == method && !n.is_abstract }
    end
  end


end
