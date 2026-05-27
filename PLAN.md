# RTSP Sink Session Notes

## Current Goal

`gst-rtsp-server` wrapper가 아니라, 파이프라인 안에서 동작하는 `GstBaseSink` 기반
RTSP sink를 표준 계층 중심 구조로 구현 중이다.

현재 기준:

- element 이름: `rtspserversink`
- 입력: `video/x-h264`
- transport: `RTP/AVP/TCP interleaved`, `RTP/AVP UDP`
- target: RTSP 제어는 표준에 가깝게 맞추고, 이후 신규 클라이언트 join 안정성을 보강

## Implemented So Far

### 1. Internal Structure

기존 단일 `src/rtspsink-server.c`를 쓰지 않고 `src/core` 기반 구조로 재구성했다.

- `src/core/rtsp-server.c`
  - server lifecycle
  - accept loop
  - client thread 관리
- `src/core/rtsp-client.c`
  - client I/O
  - transport reset
  - keepalive timestamp
- `src/core/rtsp-parser.c`
  - RTSP request line/header 파싱
  - `Content-Length` body 소비
  - `CSeq` 파싱
  - `Session` 헤더 파싱
  - `Transport` 파싱
- `src/core/rtsp-router.c`
  - `OPTIONS`, `DESCRIBE`, `SETUP`, `PLAY`, `PAUSE`, `TEARDOWN`, `GET_PARAMETER`
  - session/state validation
  - RTSP 응답 생성
- `src/core/sdp-builder.c`
  - SDP 생성
  - SPS/PPS 추출
  - `sprop-parameter-sets`, `profile-level-id` 구성
- `src/core/h264-pay.c`
  - AVC / byte-stream AU 처리
  - RTP packetization
  - FU-A fragmentation
  - STAP-A prepend
  - UDP 최소 RTCP SR/SDES 송신

### 2. RTSP / Session State

아래 상태를 내부에 도입했다.

- `INIT`
- `READY`
- `PLAYING`
- `PAUSED`
- `CLOSED`

아래 검증이 들어갔다.

- `CSeq` 필수
- `RTSP/1.0` version 검사
- `Session` header 검증
- method/state 검증
- `PAUSE` 구현
- `auth-mode != none`이면 start 실패

### 3. Legacy Cleanup

- `src/rtspsink-server.c` 삭제
- 새 `src/core`가 유일 구현

## Verified Working

아래는 확인했다.

- `meson compile -C builddir`
- `GST_PLUGIN_PATH=... gst-inspect-1.0 rtspserversink`
- 기본 pipeline 기동
- RTSP 수동 시퀀스:
  - `OPTIONS`
  - `DESCRIBE`
  - `SETUP`
  - `PLAY`
  - `PAUSE`
  - `GET_PARAMETER`
  - `TEARDOWN`
- UDP path:
  - RTP packet 수신 확인
  - RTCP SR 수신 확인
- `auth-mode=basic` 설정 시 명시적 실패 확인
- VLC에서 RTSP 연결 및 재생 시간 진행 확인

## Known Problems

가장 중요한 현재 문제:

- VLC는 연결되고 진행 시간도 증가하지만 영상은 검은 화면만 표시됨

현재 판단:

- RTSP control plane은 대체로 동작
- 문제는 media plane, 특히 신규 클라이언트가 디코딩 가능한 시작점으로
  합류하지 못하는 쪽일 가능성이 큼

가능성 높은 원인:

1. 신규 클라이언트 warm-start 부재
   - 최신 `SPS/PPS + IDR access unit`를 캐시하지 않음
   - `PLAY` 직후 디코더가 바로 시작 가능한 GOP를 받는 보장이 없음
2. parameter set 전달 타이밍 부족
   - SDP에만 있고 실제 RTP 상에서 신규 클라이언트 시점에 충분히 재주입되지 않을 수 있음
3. RTP/H264 packetization은 형식상 동작하지만 플레이어 호환성 기준으로는 아직 불충분할 수 있음
4. 현재 `RTP-Info`/`Content-Base`의 host 문제는 수정했지만, 그것만으로 검은 화면 문제는 해결되지 않음

## Important Fixes Already Applied

이번 세션에서 잡은 버그:

- `Content-Base` / `RTP-Info` URL이 `0.0.0.0`를 광고하던 문제 수정
  - 요청 URI / `Host` 기준으로 응답 URL 생성
- AVC length-prefixed NAL size를 `gsize` array에 잘못 append해서
  crash 나던 문제 수정
- stop 중 push/free 경쟁을 줄이기 위해 active pusher 동기화 추가

주의:

- 종료 시 `RTCP BYE`는 안정성 때문에 다시 제거한 상태
- 현재는 `RTCP SR/SDES`만 보냄

## Next Session Priority

다음 세션은 아래 순서로 진행하는 것이 맞다.

1. `join-safe start path` 구현
   - 최신 SPS 저장
   - 최신 PPS 저장
   - 최신 IDR access unit 저장
   - 필요하면 IDR 직전/직후 AU 묶음 저장
2. `PLAY` 직후 신규 클라이언트에 cached parameter sets + cached IDR AU 우선 송신
3. 그 다음 live RTP stream에 합류
4. VLC / ffplay / GStreamer client로 실제 영상 디코딩 확인
5. 그래도 검은 화면이면 아래를 점검
   - marker bit
   - RTP timestamp continuity
   - STAP-A 정책
   - SPS/PPS 재주입 시점
   - VLC가 기대하는 H264 packetization 형태

## Suggested Concrete Next Task

다음 작업의 직접 목표:

`PLAY` 직후 클라이언트가 첫 GOP만으로 디코딩을 시작할 수 있게
`cached SPS/PPS + cached IDR AU replay`를 구현한다.

필요한 하위 작업:

- client별 송신 큐가 없어도, 최소한 `PLAY` 직후 동기적으로 warm-start burst 전송
- server에 latest keyframe cache 추가
- H264 AU 단위 저장 구조 추가
- IDR 여부 판단 후 cache 갱신
- replay 후 live path 전환

## How To Reproduce Current State

서버 실행:

```sh
env GST_PLUGIN_PATH=/home/wittch/Public/Project/gst-rtsp-server-sink/builddir/plugin \
gst-launch-1.0 -q \
  videotestsrc is-live=true ! \
  x264enc tune=zerolatency key-int-max=15 ! \
  h264parse ! \
  rtspserversink port=9562 path=/live
```

VLC 접속:

```text
rtsp://127.0.0.1:9562/live
```

현재 기대 결과:

- 연결됨
- 재생 시간 진행
- 영상은 아직 검은 화면일 수 있음

## Files To Start With Next Time

다음 세션 시작점:

- `src/core/h264-pay.c`
- `src/core/rtsp-router.c`
- `src/core/rtsp-server-internal.h`
- `src/core/sdp-builder.c`

