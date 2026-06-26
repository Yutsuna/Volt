module Volt::Frontend


  module Suggest
    extend self

    def closest( target : String, candidates : Enumerable( String ) ) : String?
      threshold = max_distance( target )
      best      : String? = nil
      best_dist = threshold + 1

      candidates.each do |cand|
        next if cand == target
        d = distance( target, cand, best_dist )
        if d < best_dist
          best_dist = d
          best      = cand
        end
      end

      best_dist <= threshold ? best : nil
    end

    private def max_distance( target : String ) : Int32
      Math.min( 1 + target.size // 3, 3 )
    end

    private def distance( a : String, b : String, limit : Int32 ) : Int32
      ca = a.chars
      cb = b.chars
      return cb.size if ca.empty?
      return ca.size if cb.empty?

      prev = Array( Int32 ).new( cb.size + 1 ) { |i| i }
      curr = Array( Int32 ).new( cb.size + 1, 0 )

      ca.each_with_index do |ach, i|
        curr[ 0 ] = i + 1
        row_min   = curr[ 0 ]
        cb.each_with_index do |bch, j|
          cost      = ach == bch ? 0 : 1
          curr[ j + 1 ] = Math.min( Math.min( curr[ j ] + 1, prev[ j + 1 ] + 1 ), prev[ j ] + cost )
          row_min   = Math.min( row_min, curr[ j + 1 ] )
        end
        return limit + 1 if row_min > limit
        prev, curr = curr, prev
      end

      prev[ cb.size ]
    end
  end


end
