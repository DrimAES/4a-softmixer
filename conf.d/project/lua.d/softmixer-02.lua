--[[
  Copyright (C) 2016 "IoT.bzh"
  Author Fulup Ar Foll <fulup@iot.bzh>

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.


  NOTE: strict mode: every global variables should be prefixed by '_'
--]]

-- Static variables should be prefixed with _
_EventHandle={}

-- Call when AlsaCore return HAL active list
function _AlsaPingCB_ (source, result, context)

    AFB:notice (source, "--InLua-- PingCB: result='%s'", Dump_Table(result))

end


-- Display receive arguments and echo them to caller
function _mixer_config_ (source, args)
    do
    local error
    local response

    -- ==================== Default rate ===========================

    local audio_defaults = {
        ["rate"]   = 48000,
    }  

    -- ======================= Loop PCM ===========================

    local snd_aloop = {
        ["uid"]     = "Alsa-Loop",
        ["devpath"] = "/dev/snd/by-path/platform-snd_aloop.0",
        ["devices"] = {["playback"]=0,["capture"]=1},
        ["params"]  = audio_defaults,
        ["subdevs"] = {
            {["subdev"]= 0, ["numid"]= 51},
            {["subdev"]= 1, ["numid"]= 57},
            {["subdev"]= 2, ["numid"]= 63},
            {["subdev"]= 3, ["numid"]= 69},
            {["subdev"]= 4, ["numid"]= 75},
            {["subdev"]= 5, ["numid"]= 81},
            {["subdev"]= 6, ["numid"]= 87},
            {["subdev"]= 7, ["numid"]= 93},
        }
    }

    error,response= L2C:snd_loops (source, snd_aloop)
    if (error ~= 0) then 
        AFB:error (source, "--InLua-- L2C:snd_loops fail to attach sndcards=%s", Dump_Table(aloop))
        goto OnErrorExit
    else
        AFB:notice (source, "--InLua-- L2C:snd_loops done response=%s\n", Dump_Table(response))
    end
    

    -- ============================= Sound Cards ===================  

    local sndcard_0 = {
        ["uid"]= "YAMAHA-APU70",
        ["devpath"]= "/dev/snd/by-id/usb-YAMAHA_Corporation_YAMAHA_AP-U70_USB_Audio_00-00",
        ["params"] = snd_params,
        ["sink"] = {
            [0]= {["uid"]= "front-right", ["port"]= 0},
            [1]= {["uid"]= "front-left", ["port"]= 1},
        }
    }

     -- group sound card as one multi channels card
    local sndcards= {
        sndcard_0,
    }

    error,response= L2C:snd_cards (source, sndcards)
    if (error ~= 0) then 
        AFB:error (source, "--InLua-- L2C:snd_cards fail to attach sndcards=%s", Dump_Table(sndcards))
        goto OnErrorExit
    else
        AFB:notice (source, "--InLua-- L2C:snd_cards done response=%s\n", Dump_Table(response))
    end
 
    -- ============================= Zones ===================    

    local zone_front= {
        ["uid"]  = "front-seats",
        ["type"] = "playback",
        ["mapping"] = {
            {["target"]="front-right",["channel"]=0},
            {["target"]="front-left" ,["channel"]=1},
        }
    }

    local multi_zones = {
        zone_front,
    }

    error,response= L2C:snd_zones (source, multi_zones)
    if (error ~= 0) then 
        AFB:error (source, "--InLua-- L2C:snd_zones fail to attach sndcards=%s", Dump_Table(multi_zones))
        goto OnErrorExit
    else
        AFB:notice (source, "--InLua-- L2C:snd_zones done response=%s\n", Dump_Table(response))
    end

    -- =================== Audio Stream ============================

    local stream_music= {
        ["uid"]   = "multimedia",
        ["zone"]  = "front-seats",
        ["volume"]= 70,
        ["mute"]  = false,
    }
    
    local stream_navigation= {
        ["uid"]   = "navigation",
        ["zone"]  = "front-seats",
        ["volume"]= 80,
        ["mute"]  = false,
    }
        
    local snd_streams = {
        stream_music,
        stream_navigation,
    }

    error,response= L2C:snd_streams (source, snd_streams)
    if (error ~= 0) then 
        AFB:error (source, "--InLua-- L2C:snd_streams fail to attach sndcards=%s", Dump_Table(aloop))
        goto OnErrorExit
    else
        AFB:notice (source, "--InLua-- L2C:streams_loops done response=%s\n", Dump_Table(response))
    end


    -- ================== Happy End =============================
    AFB:notice (source, "--InLua-- _mixer_config_ done")
    return 0 end 

    -- ================= Unhappy End ============================
    ::OnErrorExit::
        AFB:error (source, "--InLua-- snd_attach fail")
        return 1 -- unhappy end --
end 

-- Display receive arguments and echo them to caller
function _init_softmixer_ (source, args)

    -- create event to push change audio roles to potential listeners
    _EventHandle=AFB:evtmake(source, "control")

    _mixer_config_ (source, args)

end
