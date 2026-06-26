module Volt::Frontend


  enum Severity
    Error
    Warning
    Note
    Help

    def error? : Bool
      self == Error
    end
  end


end
