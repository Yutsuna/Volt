require "colorize"
require "./ACompletionProvider"


module Volt::CLI


  # Interactive suggestions panel displayed below the prompt, in the style of the
  # `reply` shard (crystal-i): entries laid out by columns minimizing height, the
  # typed pattern highlighted inside each entry, Tab / Shift-Tab moving a reversed
  # selection through the list, and live re-filtering as the line is edited.
  class CompletionMenu

    TITLE      = "Suggestions"
    MAX_HEIGHT = 10

    getter? open = false
    getter entries = [] of String
    getter name_filter = ""
    getter replace_start = 0
    getter selection : Int32? = nil

    @all_entries = [] of String

    # Opens the menu on a fresh candidate list. `name_filter` is the word already
    # typed (entries not starting with it are hidden); `replace_start` is the char
    # index in the buffer where that word begins.
    def show( labels : Array(String), name_filter : String, replace_start : Int32 ) : Nil
      @all_entries = labels
      @replace_start = replace_start
      @open = true
      self.name_filter = name_filter
    end

    def close : Nil
      @open = false
      @selection = nil
      @entries.clear
      @all_entries.clear
      @name_filter = ""
    end

    # Re-filters entries against a new typed prefix; selection resets since the
    # visible list changed.
    def name_filter=( filter : String ) : Nil
      @name_filter = filter
      @selection = nil
      @entries = @all_entries.select( &.starts_with?( filter ) )
      close if @entries.empty?
    end

    def selection_next : String?
      return nil if @entries.empty?
      pos = @selection
      new_pos = pos.nil? ? 0 : ( pos + 1 ) % @entries.size
      @selection = new_pos
      @entries[ new_pos ]
    end

    def selection_previous : String?
      return nil if @entries.empty?
      pos = @selection
      new_pos = pos.nil? ? @entries.size - 1 : ( pos - 1 ) % @entries.size
      @selection = new_pos
      @entries[ new_pos ]
    end

    # Longest common prefix of the visible entries — what a first Tab inserts
    # before any explicit selection happens.
    def common_root : String
      return "" if @entries.empty?
      return @entries.first if @entries.size == 1

      prefix = @entries.first
      @entries.each do |entry|
        while !entry.starts_with?( prefix )
          prefix = prefix[ 0, prefix.size - 1 ]
        end
      end
      prefix
    end

    # Renders the panel: title line, then entries by columns (minimum height that
    # fits `width`), the selected entry reversed, the matched prefix in bold.
    # Horizontal window follows the selection when there are more columns than fit.
    # Returns the number of terminal lines written.
    def display( io : IO, width : Int32, max_height : Int32 = MAX_HEIGHT, color : Bool = true ) : Int32
      return 0 unless open?
      return 0 if max_height <= 1 || width <= 2

      if color
        io << TITLE.colorize.underline << ":"
      else
        io << TITLE << ":"
      end
      io << "\r\n"
      height = 1

      nb_rows = compute_nb_rows( max_height - height, width )
      columns = @entries.in_groups_of( nb_rows, filled_up_with: "" )
      column_widths = columns.map { |col| col.max_of( &.size ) + 2 }

      nb_cols = nb_columns_in_width( column_widths, width )

      col_start = 0
      if pos = @selection
        col_end = pos // nb_rows
        if col_end >= nb_cols
          nb_cols = nb_columns_in_width( column_widths[ ..col_end ].reverse_each, width )
          col_start = col_end - nb_cols + 1
        end
      end

      nb_rows.times do |r|
        nb_cols.times do |c|
          c += col_start
          entry = columns[ c ][ r ]
          col_width = column_widths[ c ]

          # `..` marks truncated columns on the last visible cell.
          if r == nb_rows - 1 && c - col_start == nb_cols - 1 && columns[ c + 1 ]?
            entry += ".."
          end

          entry_str = entry.ljust( col_width )

          if r + c * nb_rows == @selection
            if color
              io << entry_str.colorize.bright.on_dark_gray
            else
              io << ">" << entry_str[ ...-1 ]
            end
          elsif !entry.empty?
            if color && !@name_filter.empty?
              io << @name_filter.colorize.bright << entry_str.lchop( @name_filter )
            else
              io << entry_str
            end
          end
        end
        io << "\r\n"
      end

      height + nb_rows
    end

    #------------------------------------------------------------------------------------

    private def nb_columns_in_width( column_widths, width : Int32 ) : Int32
      nb_cols = 0
      w = 0
      column_widths.each do |col_width|
        w += col_width
        break if w > width
        nb_cols += 1
      end
      nb_cols
    end

    # Minimum row count that fits every column within `width` (one column per
    # entry when the list is small enough to read vertically).
    private def compute_nb_rows( max_nb_rows : Int32, width : Int32 ) : Int32
      if @entries.size > 10
        ( 1..max_nb_rows ).each do |r|
          w = 0
          @entries.each_slice( r, reuse: true ) do |col|
            w += col.max_of( &.size ) + 2
          end
          return r if w < width
        end
      end

      { @entries.size, max_nb_rows }.min
    end

  end


end
