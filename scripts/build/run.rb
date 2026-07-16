# frozen_string_literal: true


module Volt
  module Build


  $LOAD_PATH.unshift( File.expand_path( __dir__ ) )

  autoload :Logger,   'logger'
  autoload :Options,  'options'
  autoload :Cache,    'cache'
  autoload :Pipeline, 'pipeline'

  def self.run!
    options   = Options.parse
    pipeline  = Pipeline.new options
    pipeline.execute!
  end


  end
end
