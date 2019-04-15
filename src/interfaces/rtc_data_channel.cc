/* Copyright (c) 2019 The node-webrtc project authors. All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be found
 * in the LICENSE.md file in the root of the source tree. All contributing
 * project authors may be found in the AUTHORS file in the root of the source
 * tree.
 */
#include "src/interfaces/rtc_data_channel.h"

#include <utility>

#include <v8.h>
#include <webrtc/api/data_channel_interface.h>
#include <webrtc/api/scoped_refptr.h>
#include <webrtc/rtc_base/copy_on_write_buffer.h>

#include "src/enums/node_webrtc/binary_type.h"
#include "src/enums/webrtc/data_state.h"
#include "src/interfaces/rtc_peer_connection/peer_connection_factory.h"
#include "src/node/error_factory.h"
#include "src/node/events.h"

namespace node_webrtc {

Napi::FunctionReference& RTCDataChannel::constructor() {
  static Napi::FunctionReference constructor;
  return constructor;
}

DataChannelObserver::DataChannelObserver(PeerConnectionFactory* factory,
    rtc::scoped_refptr<webrtc::DataChannelInterface> jingleDataChannel)
  : _factory(factory)
  , _jingleDataChannel(std::move(jingleDataChannel)) {
  _factory->Ref();
  _jingleDataChannel->RegisterObserver(this);
}

DataChannelObserver::~DataChannelObserver() {
  _factory->Unref();
  _factory = nullptr;
}  // NOLINT

void DataChannelObserver::OnStateChange() {
  auto state = _jingleDataChannel->state();
  Enqueue(Callback1<RTCDataChannel>::Create([state](RTCDataChannel & channel) {
    RTCDataChannel::HandleStateChange(channel, state);
  }));
}

void DataChannelObserver::OnMessage(const webrtc::DataBuffer& buffer) {
  Enqueue(Callback1<RTCDataChannel>::Create([buffer](RTCDataChannel & channel) {
    RTCDataChannel::HandleMessage(channel, buffer);
  }));
}

static void requeue(DataChannelObserver& observer, RTCDataChannel& channel) {
  while (auto event = observer.Dequeue()) {
    channel.Dispatch(std::move(event));
  }
}

RTCDataChannel::RTCDataChannel(const Napi::CallbackInfo& info)
  : napi::AsyncObjectWrapWithLoop<RTCDataChannel>("RTCDataChannel", *this, info)
  , _binaryType(BinaryType::kArrayBuffer) {
  auto env = info.Env();

  if (!info.IsConstructCall()) {
    Napi::TypeError::New(env, "Use the new operator to construct the RTCDataChannel.").ThrowAsJavaScriptException();
    return;
  }

  auto _observer = v8::Local<v8::External>::Cast(napi::UnsafeToV8(info[0]));
  auto observer = static_cast<node_webrtc::DataChannelObserver*>(_observer->Value());

  _factory = observer->_factory;
  _factory->Ref();

  _jingleDataChannel = observer->_jingleDataChannel;
  _jingleDataChannel->RegisterObserver(this);

  // Re-queue cached observer events
  requeue(*observer, *this);

  delete observer;

  // NOTE(mroberts): These doesn't actually matter yet.
  _cached_id = 0;
  _cached_max_packet_life_time = 0;
  _cached_max_retransmits = 0;
  _cached_negotiated = false;
  _cached_ordered = false;
  _cached_buffered_amount = 0;
}

RTCDataChannel::~RTCDataChannel() {
  _factory->Unref();
  _factory = nullptr;

  wrap()->Release(this);
}  // NOLINT

void RTCDataChannel::CleanupInternals() {
  if (_jingleDataChannel == nullptr) {
    return;
  }
  _jingleDataChannel->UnregisterObserver();
  _cached_id = _jingleDataChannel->id();
  _cached_label = _jingleDataChannel->label();
  _cached_max_packet_life_time = _jingleDataChannel->maxRetransmitTime();
  _cached_max_retransmits = _jingleDataChannel->maxRetransmits();
  _cached_negotiated = _jingleDataChannel->negotiated();
  _cached_ordered = _jingleDataChannel->ordered();
  _cached_protocol = _jingleDataChannel->protocol();
  _cached_buffered_amount = _jingleDataChannel->buffered_amount();
  _jingleDataChannel = nullptr;
}

void RTCDataChannel::OnPeerConnectionClosed() {
  if (_jingleDataChannel != nullptr) {
    CleanupInternals();
    Stop();
  }
}

void RTCDataChannel::OnStateChange() {
  auto state = _jingleDataChannel->state();
  if (state == webrtc::DataChannelInterface::kClosed) {
    CleanupInternals();
  }
  Dispatch(CreateCallback<RTCDataChannel>([this, state]() {
    RTCDataChannel::HandleStateChange(*this, state);
  }));
}

void RTCDataChannel::HandleStateChange(RTCDataChannel& channel, webrtc::DataChannelInterface::DataState state) {
  auto env = constructor().Env();
  Napi::HandleScope scope(env);
  if (state == webrtc::DataChannelInterface::kClosed) {
    auto value = Napi::String::New(env, "closed");
    channel.MakeCallback("onstatechange", { value });
  } else if (state == webrtc::DataChannelInterface::kOpen) {
    auto value = Napi::String::New(env, "open");
    channel.MakeCallback("onstatechange", { value });
  }
  if (state == webrtc::DataChannelInterface::kClosed) {
    channel.Stop();
  }
}

void RTCDataChannel::OnMessage(const webrtc::DataBuffer& buffer) {
  Dispatch(CreateCallback<RTCDataChannel>([this, buffer]() {
    RTCDataChannel::HandleMessage(*this, buffer);
  }));
}

void RTCDataChannel::HandleMessage(RTCDataChannel& channel, const webrtc::DataBuffer& buffer) {
  bool binary = buffer.binary;
  size_t size = buffer.size();
  std::unique_ptr<char[]> message = std::unique_ptr<char[]>(new char[size]);
  memcpy(reinterpret_cast<void*>(message.get()), reinterpret_cast<const void*>(buffer.data.data()), size);

  auto env = constructor().Env();
  Napi::HandleScope scope(env);
  Napi::Value value;
  if (binary) {
    auto array = Napi::ArrayBuffer::New(env, message.release(), size, [](void* buffer, void*) {
      delete static_cast<char*>(buffer);
    });
    value = array;  // NOLINT
  } else {
    auto str = Napi::String::New(env, message.get());
    value = str;
  }
  channel.MakeCallback("onmessage", { value });
}

Napi::Value RTCDataChannel::Send(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  if (_jingleDataChannel != nullptr) {
    if (_jingleDataChannel->state() != webrtc::DataChannelInterface::DataState::kOpen) {
      // FIXME(mroberts): How to throw this?
      ErrorFactory::napi::CreateInvalidStateError(env, "RTCDataChannel.readyState is not 'open'");
      return info.Env().Undefined();
    }
    if (info[0].IsString()) {
      auto str = info[0].ToString();
      auto data = str.Utf8Value();

      webrtc::DataBuffer buffer(data);
      _jingleDataChannel->Send(buffer);
    } else {
      Napi::ArrayBuffer arraybuffer;
      size_t byte_offset = 0;
      size_t byte_length = 0;

      if (info[0].IsTypedArray()) {
        auto typedArray = info[0].As<Napi::TypedArray>();
        arraybuffer = typedArray.ArrayBuffer();
        byte_offset = typedArray.ByteOffset();
        byte_length = typedArray.ByteLength();
      } else if (info[0].IsDataView()) {
        auto dataView = info[0].As<Napi::DataView>();
        arraybuffer = dataView.ArrayBuffer();
        byte_offset = dataView.ByteOffset();
        byte_length = dataView.ByteLength();
      } else if (info[0].IsArrayBuffer()) {
        arraybuffer = info[0].As<Napi::ArrayBuffer>();
        byte_length = arraybuffer.ByteLength();
      } else {
        Napi::TypeError::New(env, "Expected a Blob or ArrayBuffer").ThrowAsJavaScriptException();
        return env.Undefined();
      }

      auto content = static_cast<char*>(arraybuffer.Data());
      rtc::CopyOnWriteBuffer buffer(content + byte_offset, byte_length);

      webrtc::DataBuffer data_buffer(buffer, true);
      _jingleDataChannel->Send(data_buffer);
    }
  } else {
    // FIXME(mroberts): How to throw this?
    ErrorFactory::napi::CreateInvalidStateError(env, "RTCDataChannel.readyState is not 'open'");
    return env.Undefined();
  }

  return env.Undefined();
}

Napi::Value RTCDataChannel::Close(const Napi::CallbackInfo& info) {
  if (_jingleDataChannel != nullptr) {
    _jingleDataChannel->Close();
  }
  return info.Env().Undefined();
}

Napi::Value RTCDataChannel::GetBufferedAmount(const Napi::CallbackInfo& info) {
  uint64_t buffered_amount = _jingleDataChannel != nullptr
      ? _jingleDataChannel->buffered_amount()
      : _cached_buffered_amount;
  CONVERT_OR_THROW_AND_RETURN_NAPI(info.Env(), buffered_amount, result, Napi::Value)
  return result;
}

Napi::Value RTCDataChannel::GetId(const Napi::CallbackInfo& info) {
  auto id = _jingleDataChannel
      ? _jingleDataChannel->id()
      : _cached_id;
  CONVERT_OR_THROW_AND_RETURN_NAPI(info.Env(), id, result, Napi::Value)
  return result;
}

Napi::Value RTCDataChannel::GetLabel(const Napi::CallbackInfo& info) {
  auto label = _jingleDataChannel != nullptr
      ? _jingleDataChannel->label()
      : _cached_label;
  CONVERT_OR_THROW_AND_RETURN_NAPI(info.Env(), label, result, Napi::Value)
  return result;
}

Napi::Value RTCDataChannel::GetMaxPacketLifeTime(const Napi::CallbackInfo& info) {
  auto max_packet_life_time = _jingleDataChannel
      ? _jingleDataChannel->maxRetransmitTime()
      : _cached_max_packet_life_time;
  CONVERT_OR_THROW_AND_RETURN_NAPI(info.Env(), max_packet_life_time, result, Napi::Value)
  return result;
}

Napi::Value RTCDataChannel::GetMaxRetransmits(const Napi::CallbackInfo& info) {
  auto max_retransmits = _jingleDataChannel
      ? _jingleDataChannel->maxRetransmits()
      : _cached_max_retransmits;
  CONVERT_OR_THROW_AND_RETURN_NAPI(info.Env(), max_retransmits, result, Napi::Value)
  return result;
}

Napi::Value RTCDataChannel::GetNegotiated(const Napi::CallbackInfo& info) {
  auto negotiated = _jingleDataChannel
      ? _jingleDataChannel->negotiated()
      : _cached_negotiated;
  CONVERT_OR_THROW_AND_RETURN_NAPI(info.Env(), negotiated, result, Napi::Value)
  return result;
}

Napi::Value RTCDataChannel::GetOrdered(const Napi::CallbackInfo& info) {
  auto ordered = _jingleDataChannel
      ? _jingleDataChannel->ordered()
      : _cached_ordered;
  CONVERT_OR_THROW_AND_RETURN_NAPI(info.Env(), ordered, result, Napi::Value)
  return result;
}

Napi::Value RTCDataChannel::GetPriority(const Napi::CallbackInfo& info) {
  std::string priority = "high";
  CONVERT_OR_THROW_AND_RETURN_NAPI(info.Env(), priority, result, Napi::Value)
  return result;
}

Napi::Value RTCDataChannel::GetProtocol(const Napi::CallbackInfo& info) {
  auto protocol = _jingleDataChannel
      ? _jingleDataChannel->protocol()
      : _cached_protocol;
  CONVERT_OR_THROW_AND_RETURN_NAPI(info.Env(), protocol, result, Napi::Value)
  return result;
}

Napi::Value RTCDataChannel::GetReadyState(const Napi::CallbackInfo& info) {
  auto state = _jingleDataChannel
      ? _jingleDataChannel->state()
      : webrtc::DataChannelInterface::kClosed;
  CONVERT_OR_THROW_AND_RETURN_NAPI(info.Env(), state, result, Napi::Value)
  return result;
}

Napi::Value RTCDataChannel::GetBinaryType(const Napi::CallbackInfo& info) {
  CONVERT_OR_THROW_AND_RETURN_NAPI(info.Env(), _binaryType, result, Napi::Value)
  return result;
}

void RTCDataChannel::SetBinaryType(const Napi::CallbackInfo& info, const Napi::Value& value) {
  auto maybeBinaryType = From<BinaryType>(value);
  if (maybeBinaryType.IsInvalid()) {
    Napi::TypeError::New(info.Env(), maybeBinaryType.ToErrors()[0]).ThrowAsJavaScriptException();
    return;
  }
  _binaryType = maybeBinaryType.UnsafeFromValid();
}

Wrap <
RTCDataChannel*,
rtc::scoped_refptr<webrtc::DataChannelInterface>,
node_webrtc::DataChannelObserver*
> * RTCDataChannel::wrap() {
  static auto wrap = new node_webrtc::Wrap <
  RTCDataChannel*,
  rtc::scoped_refptr<webrtc::DataChannelInterface>,
  node_webrtc::DataChannelObserver*
  > (RTCDataChannel::Create);
  return wrap;
}

RTCDataChannel* RTCDataChannel::Create(
    node_webrtc::DataChannelObserver* observer,
    rtc::scoped_refptr<webrtc::DataChannelInterface>) {
  auto env = constructor().Env();
  Napi::HandleScope scope(env);

  auto observerExternal = Nan::New<v8::External>(static_cast<void*>(observer));

  auto object = constructor().New({
    napi::UnsafeFromV8(env, observerExternal)
  });

  return Unwrap(object);
}

void RTCDataChannel::Init(Napi::Env env, Napi::Object exports) {
  auto func = DefineClass(env, "RTCDataChannel", {
    InstanceAccessor("bufferedAmount", &RTCDataChannel::GetBufferedAmount, nullptr),
    InstanceAccessor("id", &RTCDataChannel::GetId, nullptr),
    InstanceAccessor("label", &RTCDataChannel::GetLabel, nullptr),
    InstanceAccessor("maxPacketLifeTime", &RTCDataChannel::GetMaxPacketLifeTime, nullptr),
    InstanceAccessor("maxRetransmits", &RTCDataChannel::GetMaxRetransmits, nullptr),
    InstanceAccessor("negotiated", &RTCDataChannel::GetNegotiated, nullptr),
    InstanceAccessor("ordered", &RTCDataChannel::GetOrdered, nullptr),
    InstanceAccessor("priority", &RTCDataChannel::GetPriority, nullptr),
    InstanceAccessor("protocol", &RTCDataChannel::GetProtocol, nullptr),
    InstanceAccessor("binaryType", &RTCDataChannel::GetBinaryType, &RTCDataChannel::SetBinaryType),
    InstanceAccessor("readyState", &RTCDataChannel::GetReadyState, nullptr),
    InstanceMethod("close", &RTCDataChannel::Close),
    InstanceMethod("send", &RTCDataChannel::Send)
  });

  constructor() = Napi::Persistent(func);
  constructor().SuppressDestruct();

  exports.Set("RTCDataChannel", func);
}

}  // namespace node_webrtc
