import asyncio
import json
import websockets
import time
import logging
import tracemalloc
import numpy as np

from parse_args import args
from modelscope.pipelines import pipeline
from modelscope.utils.constant import Tasks
from modelscope.utils.logger import get_logger
from funasr.runtime.python.onnxruntime.funasr_onnx.utils.frontend import load_bytes

tracemalloc.start()

logger = get_logger(log_level=logging.CRITICAL)
logger.setLevel(logging.CRITICAL)


websocket_users = set()

print("model loading")
# asr
inference_pipeline_asr = pipeline(
    task=Tasks.auto_speech_recognition,
    model=args.asr_model,
    ngpu=args.ngpu,
    ncpu=args.ncpu,
    model_revision=None)


# vad
inference_pipeline_vad = pipeline(
    task=Tasks.voice_activity_detection,
    model=args.vad_model,
    model_revision=None,
    output_dir=None,
    batch_size=1,
    mode='online',
    ngpu=args.ngpu,
    ncpu=args.ncpu,
)

if args.punc_model != "":
    inference_pipeline_punc = pipeline(
        task=Tasks.punctuation,
        model=args.punc_model,
        model_revision=None,
        ngpu=args.ngpu,
        ncpu=args.ncpu,
    )
else:
    inference_pipeline_punc = None

inference_pipeline_asr_online = pipeline(
    task=Tasks.auto_speech_recognition,
    model=args.asr_model_online,
    ngpu=args.ngpu,
    ncpu=args.ncpu,
    model_revision='v1.0.4')

print("model loaded")

async def ws_serve(websocket, path):
    frames = []
    frames_asr = []
    frames_asr_online = []
    global websocket_users
    websocket_users.add(websocket)
    websocket.param_dict_asr = {}
    websocket.param_dict_asr_online = {"cache": dict()}
    websocket.param_dict_vad = {'in_cache': dict(), "is_final": False}
    websocket.param_dict_punc = {'cache': list()}
    websocket.vad_pre_idx = 0
    speech_start = False

    try:
        async for message in websocket:
            message = json.loads(message)
            is_finished = message["is_finished"]
            if not is_finished:
                audio = bytes(message['audio'], 'ISO-8859-1')
                frames.append(audio)
                duration_ms = len(audio)//32
                websocket.vad_pre_idx += duration_ms

                is_speaking = message["is_speaking"]
                websocket.param_dict_vad["is_final"] = not is_speaking
                websocket.param_dict_asr_online["is_final"] = not is_speaking
                websocket.param_dict_asr_online["chunk_size"] = message["chunk_size"]
                websocket.wav_name = message.get("wav_name", "demo")
                # asr online
                frames_asr_online.append(audio)
                if len(frames_asr_online) % message["chunk_interval"] == 0:
                    audio_in = b"".join(frames_asr_online)
                    await async_asr_online(websocket, audio_in)
                    frames_asr_online = []
                if speech_start:
                    frames_asr.append(audio)
                # vad online
                speech_start_i, speech_end_i = await async_vad(websocket, audio)
                if speech_start_i:
                    speech_start = True
                    beg_bias = (websocket.vad_pre_idx-speech_start_i)//duration_ms
                    frames_pre = frames[-beg_bias:]
                    frames_asr = []
                    frames_asr.extend(frames_pre)
                # asr punc offline
                if speech_end_i or not is_speaking:
                    audio_in = b"".join(frames_asr)
                    await async_asr(websocket, audio_in)
                    frames_asr = []
                    speech_start = False
                    frames_asr_online = []
                    websocket.param_dict_asr_online = {"cache": dict()}
                    if not is_speaking:
                        websocket.vad_pre_idx = 0
                        frames = []
                        websocket.param_dict_vad = {'in_cache': dict()}
                    else:
                        frames = frames[-20:]

     
    except websockets.ConnectionClosed:
        print("ConnectionClosed...", websocket_users)
        websocket_users.remove(websocket)
    except websockets.InvalidState:
        print("InvalidState...")
    except Exception as e:
        print("Exception:", e)


async def async_vad(websocket, audio_in):

    segments_result = inference_pipeline_vad(audio_in=audio_in, param_dict=websocket.param_dict_vad)

    speech_start = False
    speech_end = False
    
    if len(segments_result) == 0 or len(segments_result["text"]) > 1:
        return speech_start, speech_end
    if segments_result["text"][0][0] != -1:
        speech_start = segments_result["text"][0][0]
    if segments_result["text"][0][1] != -1:
        speech_end = True
    return speech_start, speech_end


async def async_asr(websocket, audio_in):
            if len(audio_in) > 0:
                # print(len(audio_in))
                audio_in = load_bytes(audio_in)
                
                rec_result = inference_pipeline_asr(audio_in=audio_in,
                                                    param_dict=websocket.param_dict_asr)
                # print(rec_result)
                if inference_pipeline_punc is not None and 'text' in rec_result and len(rec_result["text"])>0:
                    rec_result = inference_pipeline_punc(text_in=rec_result['text'],
                                                         param_dict=websocket.param_dict_punc)
                    # print("offline", rec_result)
                message = json.dumps({"mode": "2pass-offline", "text": rec_result["text"], "wav_name": websocket.wav_name})
                await websocket.send(message)


async def async_asr_online(websocket, audio_in):
    if len(audio_in) > 0:
        audio_in = load_bytes(audio_in)
        rec_result = inference_pipeline_asr_online(audio_in=audio_in,
                                                   param_dict=websocket.param_dict_asr_online)
        if websocket.param_dict_asr_online["is_final"]:
            websocket.param_dict_asr_online["cache"] = dict()
        if "text" in rec_result:
            if rec_result["text"] != "sil" and rec_result["text"] != "waiting_for_more_voice":
                # print("online", rec_result)
                message = json.dumps({"mode": "2pass-online", "text": rec_result["text"], "wav_name": websocket.wav_name})
                await websocket.send(message)


start_server = websockets.serve(ws_serve, args.host, args.port, subprotocols=["binary"], ping_interval=None)
asyncio.get_event_loop().run_until_complete(start_server)
asyncio.get_event_loop().run_forever()