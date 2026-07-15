module Volt::Compiler

  # Simple intra-procedural escape analysis (architecture #9).
  # Identifies registers that escape the current scope (returned, stored in the heap, etc.)
  # and removes their RAII drops so they outlive the local frame.
  module EscapeAnalysis
    def self.run( unit : Unit ) : Unit
      unit.chunks.each { |chunk| run_chunk(chunk) }
      unit
    end

    private def self.run_chunk( chunk : IR::Chunk ) : Nil
      escaped = Set(Int32).new

      # Identify direct escape sources (only outgoing stores and returns).
      chunk.code.each do |ins|
        case ins.op
        when IR::Opcode::RET
          # We only escape the returned value if the chunk's static return type is an Object.
          # If the return type is Void/Nil, we must keep dropping the last statement's registers.
          if chunk.returns_object
            n = ins.bx > 0 ? ins.bx : 1
            (0...n).each { |i| escaped << ins.a + i }
          end
        when IR::Opcode::STORE_FIELD, IR::Opcode::STORE_INDEXED
          escaped << ins.c
        when IR::Opcode::STORE_GLOBAL
          escaped << ins.a
        when IR::Opcode::STORE_PTR
          escaped << ins.b
        end
      end

      # Transitive closure: propagate escapes through moves and copies.
      changed = true
      while changed
        changed = false
        chunk.code.each do |ins|
          case ins.op
          when IR::Opcode::MOVE
            if escaped.includes?(ins.a) && !escaped.includes?(ins.b)
              escaped << ins.b
              changed = true
            elsif escaped.includes?(ins.b) && !escaped.includes?(ins.a)
              escaped << ins.a
              changed = true
            end
          when IR::Opcode::COPY_BLOCK
            n = ins.c
            (0...n).each do |i|
              dest = ins.a + i
              src = ins.b + i
              if escaped.includes?(dest) && !escaped.includes?(src)
                escaped << src
                changed = true
              elsif escaped.includes?(src) && !escaped.includes?(dest)
                escaped << dest
                changed = true
              end
            end
          end
        end
      end

      return if escaped.empty?

      # NOP out explicit DROP instructions for escaped registers.
      chunk.code.each_with_index do |ins, i|
        if ins.op == IR::Opcode::DROP && escaped.includes?(ins.a)
          chunk.code[i] = IR::Instruction.abc(IR::Opcode::NOP, 0, 0, 0)
        end
      end

      # Remove escaped registers from DropMap to prevent exception unwind drops.
      chunk.drop_map.entries.reject! { |e| escaped.includes?(e.register.to_i32) }

      # Remove escaped registers from scope tables to prevent DROP_SCOPE drops.
      chunk.scope_tables.each do |table|
        table.reject! { |r| escaped.includes?(r) }
      end
    end
  end

end
