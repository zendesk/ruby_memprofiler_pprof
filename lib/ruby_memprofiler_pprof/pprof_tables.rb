module MemprofilerPprof
  class StringTable
    def initialize
      @hash = {}
      @list = []
    end

    def index(str)
      ix = @hash[str]
      return ix if ix
      ix = @list.size
      @list << str
      @hash[str] = ix
      ix
    end

    def string_table
      Google::Protobuf::RepeatedField.new(:string, [""] + @list)
    end
  end

  class LocationTable
    def initialize(strtab)
      @strtab = strtab
      @loc_hash = {}
      @fn_hash = {}
      @counter = 1
    end

    def location_id(loc)
      path = loc.absolute_path || loc.path || "(unknown)"
      loc_tuple = [path, loc.base_label, loc.lineno]
      fn_tuple = [path, loc.base_label]

      loc_proto = @loc_hash[loc_tuple]
      return loc_proto.id if loc_proto

      fn_proto = @fn_hash[fn_tuple]
      unless fn_proto
        fn_proto = Perftools::Profiles::Function.new
        fn_proto.id = @counter
        @counter += 1
        fn_proto.name = @strtab.index(loc.base_label)
        fn_proto.system_name = fn_proto.name
        fn_proto.filename = @strtab.index(path)
        @fn_hash[fn_tuple] = fn_proto
      end

      loc_proto = Perftools::Profiles::Location.new
      loc_proto.id = @counter
      @counter += 1
      loc_proto.line << Perftools::Profiles::Line.new(
        function_id: fn_proto.id,
        line: loc.lineno,
      )
      @loc_hash[loc_tuple] = loc_proto
      loc_proto.id
    end

    def locations
      Google::Protobuf::RepeatedField.new(:message, Perftools::Profiles::Location, @loc_hash.values)
    end

    def functions
      Google::Protobuf::RepeatedField.new(:message, Perftools::Profiles::Function, @fn_hash.values)
    end
  end
end
