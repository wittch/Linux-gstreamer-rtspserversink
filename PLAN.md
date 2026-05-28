1. 기준과 범위 고정

  - 참고 구현: `BreakingY/simple-rtsp-server`
  - 표준 기준: RTSP 1.0 (`RFC 2326`)
  - 목표: GStreamer `rtspserversink` 스타일의 커스텀 sink/plugin + 내부 RTSP 서버/전송 계층 구현
  - 기본 지원 범위: `video/x-h264`, `video/x-h265`
  - 우선 transport: RTP over UDP, RTP over TCP(interleaved)
  - 기본 인증 정책: 필요 여부와 방식을 명시적으로 결정
  - 비목표를 명확히 기록: RTSP 2.0, 멀티 채널 오디오, multicast, 녹화/브릿지 기능 등

2. GStreamer 플러그인 구조 설계

  - plugin entry point와 internal library 경계 분리
  - sink element 등록명, factory name, plugin metadata 확정
  - `GstBaseSink` 기반의 pad template, caps template, metadata 정의
  - element properties와 내부 config 객체 매핑 규칙 정의
  - state change 흐름(`NULL/READY/PAUSED/PLAYING`)과 RTSP 서버 state를 분리해서 관리

3. 입력 caps 정책

  - H.264 입력 허용 조건
    - `stream-format` 허용 범위 확정: `byte-stream`, `avc`
    - `alignment` 허용 범위 확정: `au`, `nal`
    - parser 필요 여부를 caps 조합별로 정의
  - H.265 입력 허용 조건
    - `stream-format` 허용 범위 확정: `byte-stream`, `hvc1`
    - `alignment` 허용 범위 확정: `au`, `nal`
    - parser 필요 여부를 caps 조합별로 정의
  - caps negotiation 실패 시 반환 에러와 로그 정책 정의
  - 지원하지 않는 codec 또는 field 조합 처리 정책 정의

4. 내부 상태 모델

  - element 상태와 RTSP 세션 상태를 분리
  - 최소 상태 집합 정의
    - `idle`
    - `ready`
    - `playing`
    - `paused`
    - `stopping`
    - `closed`
  - client 연결 전/후 상태 전이 규칙 정의
  - stop/unlock 중 render 호출이 들어오는 경우의 처리 정의
  - late joiner가 들어왔을 때 상태 복구 규칙 정의

5. 데이터 경로와 스레드 모델

  - upstream buffer 수신 경로와 RTSP control 경로 분리
  - queue ownership, lock granularity, condition variable 사용 규칙 정의
  - client별 송신 경로와 shared media state의 경계 정의
  - write 실패, disconnect, teardown, stop 간 상호작용 정의
  - backpressure와 frame drop 정책 정의

6. RTP 출력 규격

  - payload type 할당 정책
  - clock-rate는 90000으로 고정
  - sequence number 증가 규칙
  - timestamp 산정 규칙
  - marker bit 설정 기준
  - MTU 초과 시 fragmentation 정책
  - RTP-Info 생성 규칙과 PLAY 응답 연계

7. H.264 packetization 정책

  - access unit 단위 송신 여부 확정
  - byte-stream / AVC 입력 처리 분기
  - NAL boundary와 AU boundary 분리 처리
  - FU-A 사용 조건
  - STAP-A 사용 여부와 사용 타이밍
  - SPS/PPS 보관 및 송출 시점
  - IDR 직전 parameter set 재전송 정책
  - keyframe 누락 시 복구 정책

8. H.265 packetization 정책

  - access unit 단위 송신 여부 확정
  - byte-stream / hvc1 입력 처리 분기
  - fragmentation packet 생성 정책
  - VPS/SPS/PPS 보관 및 송출 시점
  - IDR/CRA 기준 재전송 정책
  - keyframe 누락 시 복구 정책

9. SDP 생성 정책

  - session-level 과 media-level attribute 구분
  - `m=video`, `a=rtpmap`, `a=fmtp`, `a=control` 생성 규칙
  - H.264 SDP 항목 규칙
  - H.265 SDP 항목 규칙
  - `Content-Base`와 control URL 일치성 확보
  - aggregate control 허용 여부에 따른 SDP 분기

10. RTSP 제어 흐름 구현

  - `OPTIONS`, `DESCRIBE`, `SETUP`, `PLAY`, `PAUSE`, `TEARDOWN`, `GET_PARAMETER` 처리
  - `CSeq` 검증과 응답 순서 보장
  - `Session` 발급과 수명 관리
  - `Transport` 파싱 및 협상
  - `RTP-Info` 생성
  - `Range` 처리와 NPT 기준 재생 위치 정의
  - unsupported method와 unsupported transport 에러 응답 규칙 정의

11. 인증과 접근 제어

  - auth 사용 여부를 설정값으로 분리
  - 필요 시 Basic/Digest 중 채택 방식 결정
  - 인증 실패 시 응답 코드와 헤더 처리 정의
  - 세션별 권한 검증이 필요한지 여부 정의
  - URL path 기반 접근 제어 규칙 정의

12. late joiner와 재연결 안정성

  - 최신 SPS/PPS/VPS 캐시
  - 최신 keyframe 캐시
  - PLAY 직후 warm-start 전송 정책
  - 중간 합류 client의 디코딩 보장 조건
  - 재연결 시 이전 session 정리와 새 session 생성 규칙

13. 버퍼/프레임 경계 처리

  - access unit 판별 기준
  - timestamp, duration, flags 반영 규칙
  - segment event 반영 방식
  - discontinuity, gap, flush 처리 정책
  - parser가 AU를 완전히 보장하지 않을 때 보정 로직 정의

14. 에러 처리 정책

  - 잘못된 caps
  - 미지원 codec
  - RTSP state mismatch
  - transport negotiation 실패
  - socket write 실패
  - client disconnect
  - upstream EOS와 downstream session 종료의 연결 방식

15. 참고 구현과 표준 정합성 점검

  - `simple-rtsp-server`의 RTSP 1.0 흐름과 비교
  - 파일 재생, live session, auth, UDP/TCP handling 구조 참고
  - RFC 2326의 request/response header와 상태 코드 매핑 점검
  - SDP의 `a=control` 규칙 점검
  - RTP payload 규격과 실제 packetization 일치성 점검
  - `simple-rtsp-server`보다 더 엄격한 GStreamer sink contract를 유지하는지 확인

16. 구현 파일과 빌드 골격

  - `src/` 내부 모듈 분리 기준 확정
  - parser, packetizer, SDP builder, RTSP server, router, client, config 파일 배치 확정
  - plugin entry와 internal library 연결 검증
  - `meson` 빌드가 실제 소스 구조와 일치하는지 유지
  - 최소한의 정적 분석과 컴파일 경고 기준 정의

17. 로컬 검증 시나리오

  - H.264 입력 테스트
  - H.265 입력 테스트
  - VLC 재생 확인
  - `rtspsrc` 재생 확인
  - UDP 재생 확인
  - TCP interleaved 재생 확인
  - 중간 join, 재연결, 반복 start-stop 안정성 확인
  - late joiner에서 keyframe 복구 확인
  - auth on/off 동작 확인

18. 문서화

  - 지원 caps
  - 권장 pipeline 예시
  - RTSP URL 예시
  - parser 필요 여부
  - transport 지원 범위
  - 인증 방식
  - known issue
  - 표준에서 의도적으로 제한한 항목
