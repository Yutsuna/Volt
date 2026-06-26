module Volt::Frontend


  enum Prec : Int32
    None       =  0
    Assignment =  1   # =  :
    Or         =  2   # or  ||
    And        =  3   # and  &&
    Equality   =  4   # ==  !=
    Comparison =  5   # <  >  <=  >=
    Range      =  6   # ..  ...
    Pipe       =  7   # |>
    Term       =  8   # +  -
    Factor     =  9   # *  /  %
    Power      = 10   # **  (right-associative: recurse with Power - 1)
    Unary      = 11   # !  not  -(prefix)
    Call       = 12   # ()  []  .  ?.
    Primary    = 13
  end


end
