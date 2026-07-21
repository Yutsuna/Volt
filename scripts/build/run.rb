# frozen_string_literal: true

require_relative 'scheduler'


module Volt
  module Build


    $LOAD_PATH.unshift( File.expand_path( __dir__ ) )

    autoload :Logger,   'logger'
    autoload :Options,  'options'
    autoload :Cache,    'cache'
    autoload :Pipeline, 'pipeline'


    def self.run!
      parse_result = Options.parse
      Scheduler.run!( parse_result )
    end


  end
end
