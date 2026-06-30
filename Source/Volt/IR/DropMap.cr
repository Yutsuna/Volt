module Volt::IR


  struct DropEntry
    property pc_start : UInt32
    property pc_end   : UInt32
    property register : UInt8
    property type_id  : Int32

    def initialize( @pc_start : UInt32, @pc_end : UInt32, @register : UInt8, @type_id : Int32)
    end
  end


  struct DropMap
    property entries : Array(DropEntry)

    def initialize
      @entries = [] of DropEntry
    end

    def empty? : Bool
      @entries.empty?
    end
  end


end
