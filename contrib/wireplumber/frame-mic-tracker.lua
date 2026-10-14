-- SPDX-License-Identifier: MIT
-- Steam Frame のマイクのフィルターを、設定に合わせてオン・オフする WirePlumber スクリプト。
-- Valve の /etc/wireplumber/scripts/microphone-tracker.lua の代わりに動く（元のファイルは書き換えない）。
--
-- 元のスクリプトと同じく、マイクを使うアプリがいる間だけフィルター（EQ・エコー除去・ノイズ除去）を有効にする。
-- 違いは、エコー除去とノイズ除去をそれぞれ WirePlumber の設定で切れること。
--   wpctl settings --save frame-mic.echo-cancel false       # エコー除去を切る（すぐ反映・再起動後も残る）
--   wpctl settings --save frame-mic.noise-suppression true  # ノイズ除去を入れる
-- 設定を変えると、使用中のマイクにもその場で反映する（PipeWire の再起動は要らない）。

local state = {
  stream_count = 0,
}

-- フィルターのノード（Valve の設定で steamos.mic_filter = true が付いているもの）
local filters_om = ObjectManager {
  Interest {
    type = "node",
    -- ノードの情報側のプロパティなので type = "pw" が要る（既定の pw-global だと一致しない）
    Constraint { "steamos.mic_filter", "=", "true", type = "pw" },
  }
}

-- フィルターの有効・無効を書き込む先（filters メタデータ）
local metadata_om = ObjectManager {
  Interest {
    type = "metadata",
    Constraint { "metadata.name", "=", "filters" },
  }
}

-- ノード名から、どの設定でオン・オフするかを決める
-- 戻り値: 設定のキー。nil なら常にマイクの使用状況だけで決める（EQ など）
local function setting_for (node)
  local name = node.properties ["node.name"] or ""
  if name:find ("^echo_cancel") then
    return "frame-mic.echo-cancel"
  end
  if name:find ("^ns_") or name:find ("^dsp_") then
    return "frame-mic.noise-suppression"
  end
  return nil
end

-- 1 つのフィルターを今の状態に合わせる
local function apply_node (metadata, node)
  local disabled = state.stream_count == 0
  local key = setting_for (node)
  if not disabled and key ~= nil and not Settings.get_boolean (key) then
    disabled = true
  end
  local node_id = node.properties ["object.id"]
  metadata:set (node_id, "filter.smart.disabled", "Spa:String:JSON",
      disabled and "true" or "false")
end

-- すべてのフィルターを今の状態に合わせる
local function apply_all ()
  local metadata = metadata_om:lookup ()
  if metadata == nil then
    return
  end
  for node in filters_om:iterate () do
    apply_node (metadata, node)
  end
end

filters_om:connect ("object-added", function (_, node)
  local metadata = metadata_om:lookup ()
  if metadata ~= nil then
    apply_node (metadata, node)
  end
end)
metadata_om:connect ("object-added", function ()
  apply_all ()
end)

filters_om:activate ()
metadata_om:activate ()

Settings.subscribe ("frame-mic.*", function ()
  apply_all ()
end)

SimpleEventHook {
  name = "frame-mic-stream-added",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "node-added" },
      Constraint { "media.class", "=", "Stream/Input/Audio" },
      Constraint { "node.virtual", "!", "true" },
    }
  },
  execute = function (event)
    state.stream_count = state.stream_count + 1
    if state.stream_count == 1 then
      apply_all ()
    end
  end
}:register ()

-- PC への無線マイク（VRLink）を、ミックスがオンのときだけ「Frame Mic + Mix」から取らせる。
-- ソースが無い・設定がオフのときは、自分で向けたターゲットだけ外して、ふつうの既定マイクに戻す。
local mix_source_name = "frame_micmix_source"
local routed = {}

local default_om = ObjectManager {
  Interest {
    type = "metadata",
    Constraint { "metadata.name", "=", "default" },
  }
}

local capture_om = ObjectManager {
  Interest {
    type = "node",
    Constraint { "media.class", "=", "Stream/Input/Audio" },
  }
}

local source_om = ObjectManager {
  Interest {
    type = "node",
    Constraint { "node.name", "=", mix_source_name },
  }
}

local function is_vrlink (node)
  local props = node.properties
  local media = props["media.name"] or ""
  local app = props["application.name"] or ""
  local binary = props["application.process.binary"] or ""
  return media == "Valve VRLink Microphone Input" or app == "vrlink" or binary == "vrlink"
end

local function apply_route ()
  local metadata = default_om:lookup()
  if metadata == nil then
    return
  end
  local mix_on = Settings.get_boolean("frame-mic.mix")
  local source = source_om:lookup()
  for node in capture_om:iterate() do
    if is_vrlink(node) then
      local id = tonumber(node.properties["object.id"])
      if id ~= nil then
        if mix_on and source ~= nil then
          local serial = source.properties["object.serial"]
          if serial ~= nil then
            -- 名前の JSON だと既定マイクのままになる。object.serial の数字だけがリンク先になる。
            metadata:set(id, "target.object", nil, tostring(serial))
            routed[id] = true
          end
        elseif routed[id] then
          metadata:set(id, "target.object", nil, nil)
          routed[id] = nil
        end
      end
    end
  end
end

default_om:connect("object-added", function ()
  apply_route()
end)
capture_om:connect("object-added", function ()
  apply_route()
end)
source_om:connect("object-added", function ()
  apply_route()
end)
source_om:connect("object-removed", function ()
  apply_route()
end)

default_om:activate()
capture_om:activate()
source_om:activate()

Settings.subscribe("frame-mic.mix", function ()
  apply_route()
end)
apply_route()

-- Frame のスピーカーが既定のとき、SteamOS は再生ストリームを空間オーディオへ奪う。
-- アプリが「Frame Mix: chN」を出力に選んでいたら、そのターゲットをメタデータで戻す。
local playback_om = ObjectManager {
  Interest {
    type = "node",
    Constraint { "media.class", "=", "Stream/Output/Audio" },
  }
}

local function channel_target (node)
  local props = node.properties
  local target = props["target.object"] or props["node.target"] or ""
  if target:find("^frame_mix_ch[1-8]$") then
    return target
  end
  return nil
end

local sink_om = ObjectManager {
  Interest {
    type = "node",
    Constraint { "media.class", "=", "Audio/Sink" },
  }
}

local function sink_serial (name)
  for node in sink_om:iterate() do
    if node.properties["node.name"] == name then
      return node.properties["object.serial"]
    end
  end
  return nil
end

-- 名前（frame_mix_ch1）ではなく object.serial を書く。名前の JSON 文字列だと
-- リンク先に選ばれず、既定のスピーカーへ落ちる。
local function pin_channel_targets ()
  local metadata = default_om:lookup()
  if metadata == nil then
    return
  end
  for node in playback_om:iterate() do
    local target = channel_target(node)
    local id = tonumber(node.properties["object.id"])
    local serial = target and sink_serial(target) or nil
    if serial ~= nil and id ~= nil then
      -- 型なしの数字。JSON 文字列にするとリンク先として使われない。
      metadata:set(id, "target.object", nil, tostring(serial))
    end
  end
end

playback_om:connect("object-added", function ()
  pin_channel_targets()
end)
sink_om:connect("object-added", function ()
  pin_channel_targets()
end)
playback_om:activate()
sink_om:activate()

SimpleEventHook {
  name = "frame-mic-stream-removed",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "node-removed" },
      Constraint { "media.class", "=", "Stream/Input/Audio" },
      Constraint { "node.virtual", "!", "true" },
    }
  },
  execute = function (event)
    state.stream_count = math.max (0, state.stream_count - 1)
    if state.stream_count == 0 then
      apply_all ()
    end
  end
}:register ()
