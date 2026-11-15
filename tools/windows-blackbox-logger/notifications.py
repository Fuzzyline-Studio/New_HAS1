"""Optional incident notifications. No credentials, raw responses or URLs in errors.

Called only by the durable outbox worker; never by MQTT ingestion/ACK handling.
HTTP runs in an isolated short-lived child so DNS and slow reads also have a deadline.
"""
from dataclasses import asdict, dataclass
from datetime import datetime, timedelta, timezone
import http.client
import json
from pathlib import Path
import re
import subprocess
import sys
import time
from urllib.parse import urlsplit
import uuid

from service_runtime import read_machine_password

NOTION_VERSION = "2025-09-03"
MAX_BODY = 65536
TOTAL_TIMEOUT = 10.0
# A reboot is over before it is reported, so its event is filed as recovered.
NOTION_STATUS_NAMES = {"offline": "장애중", "recovered": "복구됨", "monitoring_ended": "미복구 종료", "rebooted": "복구됨"}
# Device-event DB (configured data source) and the incident DB its "게임 세션" relation points to.
NOTION_EVENT_PROPERTIES = {"이벤트": "title", "사건 ID": "rich_text", "상태": "status", "게임 세션": "relation",
                           "장치": "select", "역할": "select", "매장": "select", "발생 시각": "date",
                           "종료 시각": "date", "종료 사유": "select", "지속(초)": "number",
                           "MAC": "rich_text", "요약": "rich_text"}
NOTION_SESSION_PROPERTIES = {"장애": "title", "세션 키": "rich_text", "매장": "select",
                             "게임 세션 ID": "rich_text", "처음 발생": "date"}
NOTION_CREATE_RESERVE = 5.0  # seconds left in the budget before an event create is attempted
EVIDENCE_HEADING = "📦 블랙박스 증거"
NOTION_SESSION_CACHE = 256
SLACK_TOKEN = re.compile(r"xoxb-[A-Za-z0-9-]{10,250}")
SLACK_CHANNEL = re.compile(r"[CG][A-Z0-9]{8,20}")
SLACK_TS = re.compile(r"\d{1,12}\.\d{1,8}")
# Slack documents these as transient; all but service_unavailable may have partially completed.
SLACK_TRANSIENT = {"internal_error", "fatal_error", "request_timeout", "service_unavailable"}
SLACK_ORIGINAL_GONE = {"slack_message_not_found", "slack_cant_update_message"}


@dataclass(frozen=True)
class DeliveryResult:
    state: str
    code: str
    retry_after: int = 0
    remote_id: str | None = None
    # The game-session object this delivery wrote under: Slack card ts or Notion session page id.
    parent_id: str | None = None


def _uuid(value):
    try:
        return str(uuid.UUID(value))
    except (ValueError, TypeError, AttributeError):
        raise ValueError("Invalid Notion identifier") from None


def validate_config(value, base_path):
    if value is None:
        value = {}
    if not isinstance(value, dict) or set(value) - {"enabled", "credential_file", "slack", "notion"}:
        raise ValueError("Invalid notifications configuration")
    result = {"enabled": value.get("enabled", False)}
    if type(result["enabled"]) is not bool:
        raise ValueError("Invalid notifications enabled flag")
    for channel in ("slack", "notion"):
        entry = value.get(channel, {})
        keys = {"enabled"} | ({"data_source_id"} if channel == "notion" else {"channel_id"})
        if not isinstance(entry, dict) or set(entry) - keys or type(entry.get("enabled", False)) is not bool:
            raise ValueError("Invalid notification channel configuration")
        result[channel] = {"enabled": entry.get("enabled", False)}
        if channel == "notion" and result["enabled"] and entry.get("enabled"):
            result[channel]["data_source_id"] = _uuid(entry.get("data_source_id"))
        if channel == "slack" and "channel_id" in entry:
            # Present only for the bot-token mode that threads recoveries; absent means webhook.
            if not isinstance(entry["channel_id"], str) or not SLACK_CHANNEL.fullmatch(entry["channel_id"]):
                raise ValueError("Invalid Slack channel ID")
            result[channel]["channel_id"] = entry["channel_id"]
    if result["enabled"]:
        path = value.get("credential_file")
        if not isinstance(path, str) or not path.strip() or "\0" in path:
            raise ValueError("Notification credential_file is required")
        result["credential_file"] = str((Path(base_path) / path).resolve())
        if not any(result[c]["enabled"] for c in ("slack", "notion")):
            raise ValueError("Enable at least one notification channel")
    return result


def _allowed_url(url, method):
    try:
        parsed = urlsplit(url)
        if (parsed.scheme != "https" or parsed.username or parsed.password or
                parsed.port not in (None, 443) or parsed.query or parsed.fragment):
            raise ValueError()
        if parsed.hostname == "hooks.slack.com":
            valid = method == "POST" and re.fullmatch(r"/services/[A-Za-z0-9_-]+/[A-Za-z0-9_-]+/[A-Za-z0-9_-]+", parsed.path)
        elif parsed.hostname == "slack.com":
            valid = method == "POST" and parsed.path in ("/api/chat.postMessage", "/api/chat.update")
        elif parsed.hostname == "api.notion.com":
            valid = ((method == "POST" and (parsed.path == "/v1/pages" or
                     re.fullmatch(r"/v1/data_sources/[0-9a-f-]{36}/query", parsed.path))) or
                     (method == "PATCH" and re.fullmatch(r"/v1/(?:pages/[0-9a-f-]{36}|blocks/[0-9a-f-]{36}/children)", parsed.path)) or
                     (method == "GET" and re.fullmatch(r"/v1/(?:databases/[0-9a-f-]{36}|data_sources/[0-9a-f-]{36}|blocks/[0-9a-f-]{36}/children)", parsed.path)))
        else:
            valid = False
        if not valid:
            raise ValueError()
        return parsed
    except (ValueError, TypeError):
        raise ValueError("Invalid notification destination") from None


def _http_child(request):
    """Fixed hosts, TLS verification, no redirects or proxy environment use."""
    parsed = _allowed_url(request["url"], request["method"])
    data = None if request["method"] == "GET" else json.dumps(request["body"], ensure_ascii=False).encode("utf-8")
    if data is not None and len(data) > MAX_BODY:
        raise ValueError("Request too large")
    headers = {"Content-Type": "application/json", "User-Agent": "HAS1BlackBox/1"}
    if parsed.hostname == "api.notion.com":
        token = request["token"]
        if not isinstance(token, str) or not re.fullmatch(r"[A-Za-z0-9_-]{10,512}", token):
            raise ValueError("Invalid credential")
        headers.update({"Authorization": "Bearer " + token, "Notion-Version": NOTION_VERSION})
    elif parsed.hostname == "slack.com":
        token = request["token"]
        if not isinstance(token, str) or not SLACK_TOKEN.fullmatch(token):
            raise ValueError("Invalid credential")
        headers.update({"Authorization": "Bearer " + token, "Content-Type": "application/json; charset=utf-8"})
    connection = http.client.HTTPSConnection(parsed.hostname, timeout=TOTAL_TIMEOUT)
    try:
        connection.request(request["method"], parsed.path, data, headers)
        response = connection.getresponse()
        body = response.read(MAX_BODY + 1)
        if len(body) > MAX_BODY:
            return {"status": response.status, "oversized": True}
        return {"status": response.status, "body": body.decode("utf-8", errors="replace"),
                "retry_after": response.getheader("Retry-After", "")[:128]}
    finally:
        connection.close()


def _transport(request, timeout):
    try:
        completed = subprocess.run([sys.executable, "-E", "-s", str(Path(__file__).resolve()), "--http-worker"],
                                   input=json.dumps(request), text=True, capture_output=True,
                                   timeout=max(0.01, timeout), check=False,
                                   creationflags=(getattr(subprocess, "CREATE_NO_WINDOW", 0)))
        if completed.returncode != 0:
            return {"network_error": True}
        result = json.loads(completed.stdout)
        return result if isinstance(result, dict) else {"network_error": True}
    except (subprocess.TimeoutExpired, OSError, ValueError):
        return {"network_error": True}


def _classify(response, *, safe_retry=False):
    if response.get("network_error"):
        return DeliveryResult("retryable" if safe_retry else "unknown", "network_or_timeout", 30)
    status = response.get("status", 0)
    if status in (429, 529):
        try:
            blocked = json.loads(response.get("body", "{}")).get("additional_data", {}).get("rate_limit_reason") == "public_api_request_blocked"
        except (ValueError, AttributeError):
            blocked = False
        if blocked:
            return DeliveryResult("permanent", "api_access_blocked")
        try:
            delay = max(1, min(86400, int(response.get("retry_after", "30"))))
        except (ValueError, TypeError):
            delay = 30
        return DeliveryResult("retryable", "rate_limited", delay)
    if 200 <= status < 300:
        return None
    # Notion documents 409 conflict_error as a collision to retry.
    if status >= 500 or status in (408, 409):
        return DeliveryResult("retryable" if safe_retry else "unknown", "remote_unavailable", 30)
    return DeliveryResult("permanent", "http_" + str(status))


def notion_database_id(url):
    """Parse a copied Notion DB URL; never connect to the supplied URL itself."""
    try:
        parsed = urlsplit(url)
        if (parsed.scheme != "https" or parsed.hostname not in ("notion.so", "www.notion.so", "app.notion.com") or
                parsed.username or parsed.password or parsed.port not in (None, 443)):
            raise ValueError()
        segment = parsed.path.rstrip("/").rsplit("/", 1)[-1]
        match = re.search(r"([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}|[0-9a-fA-F]{32})$", segment)
        if not match:
            raise ValueError()
        return _uuid(match.group(1))
    except (ValueError, TypeError):
        raise ValueError("Invalid Notion database URL") from None


def resolve_notion_database(database_url, token, *, transport=_transport):
    """Read-only discovery. Multiple sources require explicit operator selection."""
    try:
        database_id = notion_database_id(database_url)
        if not isinstance(token, str) or not re.fullmatch(r"[A-Za-z0-9_-]{10,512}", token):
            return DeliveryResult("permanent", "invalid_credentials")
        response = transport({"url": "https://api.notion.com/v1/databases/" + database_id,
                              "method": "GET", "body": None, "token": token}, TOTAL_TIMEOUT)
        failure = _classify(response, safe_retry=True)
        if failure:
            return failure
        if response.get("oversized"):
            return DeliveryResult("permanent", "oversized_database_response")
        data = json.loads(response.get("body", ""))
        sources = data.get("data_sources")
        if not isinstance(sources, list) or not sources:
            return DeliveryResult("permanent", "no_data_sources")
        if len(sources) != 1:
            return DeliveryResult("permanent", "multiple_data_sources_choose_explicit_id")
        return DeliveryResult("delivered", "data_source_resolved", remote_id=_uuid(sources[0]["id"]))
    except (ValueError, TypeError, AttributeError, KeyError, OSError, RuntimeError):
        return DeliveryResult("permanent", "invalid_database_or_credentials")


def _schema_properties(response, source_id, expected):
    """Validate only the properties the collector writes; never repair a remote schema."""
    failure = _classify(response, safe_retry=True)
    if failure:
        return None, failure
    if response.get("oversized"):
        return None, DeliveryResult("permanent", "oversized_schema_response")
    try:
        data = json.loads(response.get("body", ""))
        if (not isinstance(data, dict) or data.get("object") != "data_source" or
                _uuid(data.get("id")) != source_id or not isinstance(data.get("properties"), dict)):
            raise ValueError()
        properties = data["properties"]
    except (ValueError, TypeError):
        return None, DeliveryResult("retryable", "invalid_schema_response", 30)
    for name, kind in expected.items():
        prop = properties.get(name)
        if not isinstance(prop, dict) or prop.get("type") != kind or not isinstance(prop.get(kind), dict):
            return None, DeliveryResult("permanent", "notion_schema_mismatch")
    return properties, None


def _notion_schema(response, source_id):
    """Validate the device-event source; returns the incident (game session) source it relates to."""
    properties, failure = _schema_properties(response, source_id, NOTION_EVENT_PROPERTIES)
    if failure:
        return None, failure
    options = properties["상태"]["status"].get("options")
    if not isinstance(options, list) or any(not isinstance(option, dict) or not isinstance(option.get("name"), str) for option in options):
        return None, DeliveryResult("retryable", "invalid_schema_response", 30)
    # Validate all transitions during setup as well as delivery. No case folding,
    # option creation or fallback to 복구됨: monitoring ended is not recovery.
    names = [option["name"] for option in options]
    if any(names.count(name) != 1 for name in NOTION_STATUS_NAMES.values()):
        return None, DeliveryResult("permanent", "notion_status_options_mismatch")
    try:
        return _uuid(properties["게임 세션"]["relation"].get("data_source_id")), None
    except ValueError:
        return None, DeliveryResult("permanent", "notion_schema_mismatch")


def _read_notion_schemas(call, source_id, token):
    response = call(f"https://api.notion.com/v1/data_sources/{source_id}", "GET", None, token)
    session_source, failure = _notion_schema(response, source_id)
    if failure:
        return None, failure
    response = call(f"https://api.notion.com/v1/data_sources/{session_source}", "GET", None, token)
    _, failure = _schema_properties(response, session_source, NOTION_SESSION_PROPERTIES)
    return (None, failure) if failure else (session_source, None)


def check_notion_schema(data_source_id, token, *, transport=_transport):
    """Read-only installation preflight of both sources, with no credentials in the result."""
    try:
        source_id = _uuid(data_source_id)
        if not isinstance(token, str) or not re.fullmatch(r"[A-Za-z0-9_-]{10,512}", token):
            return DeliveryResult("permanent", "invalid_credentials")
        def call(url, method, body, token):
            return transport({"url": url, "method": method, "body": body, "token": token}, TOTAL_TIMEOUT)
        _, failure = _read_notion_schemas(call, source_id, token)
        return failure or DeliveryResult("delivered", "schema_valid_status")
    except (ValueError, TypeError, AttributeError, KeyError, OSError, RuntimeError):
        return DeliveryResult("permanent", "invalid_schema_or_credentials")


def _text(value, limit=1800):
    # No arbitrary nested payload dumps: external destinations receive only selected fields.
    return str(value or "").replace("\0", "")[:limit]


KST = timezone(timedelta(hours=9), "KST")
SLACK_HEADERS = {"offline": "🔴 장치 연결 끊김", "recovered": "🟢 장치 복구됨",
                 "monitoring_ended": "⚪ 모니터링 종료 (복구 확인 안 됨)", "rebooted": "🔁 장치 재부팅"}
CLOSE_REASONS = {"reconnected": "장치 재연결", "snapshot_no_longer_active": "게임 종료 또는 서버 목록에서 제외",
                 "game_ended": "게임 종료", "server_restarted": "서버 재시작", "watch_removed": "감시 대상에서 제외"}
# generator_watch reboot labels (esp_reset_reason_t).
REBOOT_LABELS = {"panic": "패닉(크래시)", "int_wdt": "인터럽트 워치독", "task_wdt": "태스크 워치독", "wdt": "워치독",
                 "brownout": "전압 저하", "pwr_glitch": "전원 글리치", "cpu_lockup": "CPU 잠김",
                 "poweron": "전원 투입", "ext": "외부 리셋", "sw": "소프트웨어",
                 "scan_fault": "Wi-Fi 스캔 정지 복구"}


def _parse_time(value):
    try:
        stamp = datetime.fromisoformat(str(value).replace("Z", "+00:00"))
        stamp = stamp if stamp.tzinfo else stamp.replace(tzinfo=timezone.utc)
        return stamp.astimezone(timezone.utc)
    except (ValueError, TypeError, OverflowError):
        return None


def _kst(value):
    try:
        stamp = _parse_time(value)
        return stamp.astimezone(KST).strftime("%m/%d %H:%M:%S") if stamp else None
    except OverflowError:
        return None


def _fallback(text):
    # Top-level text is mrkdwn even when blocks are plain_text; keep values inert there too.
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")[:300]


def _duration(seconds):
    if seconds is None or seconds < 0:
        return None
    seconds = int(seconds)
    if seconds < 60:
        return f"{seconds}초"
    if seconds < 3600:
        return f"{seconds // 60}분 {seconds % 60}초"
    return f"{seconds // 3600}시간 {seconds % 3600 // 60}분"


def _outage(payload):
    opened, occurred = _parse_time(payload.get("opened_at")), _parse_time(payload.get("occurred_at"))
    return _duration((occurred - opened).total_seconds()) if opened and occurred else None


def _close_reason(payload):
    reason = _text(payload.get("close_reason"), 80)
    return CLOSE_REASONS.get(reason, reason)


def _reboot_reason(payload):
    label = _text(payload.get("reset_label"), 40)
    return REBOOT_LABELS.get(label, label)


def slack_message(payload):
    """Human-readable card. plain_text only, so payload values can never mention or link."""
    status = payload.get("status")
    header = SLACK_HEADERS.get(status, "HAS1 장치 장애")
    device = _text(payload.get("device_id"), 60) or "알 수 없는 장치"
    site = _text(payload.get("site_id"), 60)
    try:
        gap = _duration(int(payload.get("gap_ms")) / 1000)
    except (ValueError, TypeError, OverflowError):
        gap = None
    fields = [("매장", site), ("장치", device), ("역할", _text(payload.get("role"), 60)),
              ("MAC", _text(payload.get("device_mac"), 40))]
    if status == "offline":
        fields += [("끊긴 시각", _kst(payload.get("occurred_at"))),
                   ("마지막 수신", _kst(payload.get("last_seen_utc"))), ("무응답 시간", gap)]
    elif status == "rebooted":
        fields += [("확인 시각", _kst(payload.get("occurred_at"))), ("재부팅 사유", _reboot_reason(payload)),
                   ("펌웨어", _text(payload.get("firmware"), 20))]
    else:
        fields += [("끊긴 시각", _kst(payload.get("opened_at"))),
                   ("복구 시각" if status == "recovered" else "종료 시각", _kst(payload.get("occurred_at"))),
                   ("장애 지속", _outage(payload)), ("종료 사유", _close_reason(payload))]
    fields.append(("게임 세션", _text(payload.get("game_session_id"), 80)))
    blocks = [{"type": "header", "text": {"type": "plain_text", "text": f"{header} · {device}"[:150]}},
              {"type": "section", "fields": [{"type": "plain_text", "text": f"{label}\n{value}"[:2000]}
                                             for label, value in fields if value and value != "unknown"][:10]}]
    note = "원인은 아직 확정되지 않았습니다." if status in ("offline", "rebooted") else ""
    blocks.append({"type": "context", "elements": [{"type": "plain_text",
                   "text": f"{note} 사건 ID {_text(payload.get('incident_id'), 160)}".strip()}]})
    title = " ".join(part for part in (header, site, device) if part)
    return {"text": _fallback(title), "blocks": blocks, "unfurl_links": False, "unfurl_media": False}


SESSION_STATES = {"open": "🔴", "recovered": "🟢", "closed": "⚪"}
SESSION_LINES = 12


def _notion_link(page_id, label):
    # The only mrkdwn the collector sends: a fixed label and a URL built from a validated UUID.
    try:
        return {"type": "mrkdwn", "text": f"<https://www.notion.so/{_uuid(page_id).replace('-', '')}|{label}>"}
    except ValueError:
        return None


def _session_item_line(item):
    device = _text(item.get("device_id"), 40) or "알 수 없는 장치"
    if item.get("reason") == "generator_reboot":
        return f"🔁 {device} · {_kst(item.get('opened_utc')) or '?'} 재부팅"
    opened, updated = _parse_time(item.get("opened_utc")), _parse_time(item.get("updated_utc"))
    line = f"{SESSION_STATES.get(item.get('state'), '❔')} {device} · {_kst(item.get('opened_utc')) or '?'} 끊김"
    # A generator whose outage was a reboot: one event, so one line. A group names the member.
    try:
        reboots = json.loads(item["reboots"]) if isinstance(item.get("reboots"), str) else item.get("reboots")
    except ValueError:
        reboots = None
    rebooted = ""
    for entry in reboots[:5] if isinstance(reboots, list) else []:
        if isinstance(entry, dict):
            name = _text(entry.get("name"), 20)
            parts = (name if name != device else None, _reboot_reason({"reset_label": entry.get("label")}), "재부팅")
            rebooted += " · " + " ".join(part for part in parts if part)
    if item.get("state") == "recovered":
        outage = _duration((updated - opened).total_seconds()) if opened and updated else None
        return line + (f" → {outage} 만에 복구" if outage else " → 복구") + rebooted
    return line + (" → 미복구 종료" if item.get("state") == "closed" else " → 장애중") + rebooted


def _session_counts(session):
    """(open, recovered, unrecovered) over the whole session, not just the listed lines."""
    counts = session.get("counts")
    if not isinstance(counts, dict):
        states = [item.get("state") for item in session.get("incidents") or [] if isinstance(item, dict)]
        counts = {state: states.count(state) for state in set(states)}
    def number(state):
        value = counts.get(state, 0)
        return value if type(value) is int and value >= 0 else 0
    return number("open"), number("recovered"), number("closed")


def _session_label(game):
    # The collector's own day session for events outside games reads as a date, not an ID.
    day = re.fullmatch(r"게임외-\d{4}(\d{2})(\d{2})", game or "")
    return f"게임 외 {day[1]}/{day[2]}" if day else f"세션 {game}" if game else None


def _kst_since(value, base):
    """KST time of value, without its date when that is the date of base."""
    shown, since = _kst(value), _kst(base)
    return shown[6:] if shown and since and shown[:5] == since[:5] else shown


def slack_session_card(payload, session):
    """One incident card per game session, edited in place like PagerDuty/Opsgenie alerts.

    A short title, which game it was, a summary of its incidents, the device lines, then where to
    look next. plain_text only, except the Notion link whose URL and label the collector builds itself.
    """
    items = [item for item in session.get("incidents") or [] if isinstance(item, dict)]
    open_count, recovered, unrecovered = _session_counts(session)
    total = open_count + recovered + unrecovered
    site = _text(session.get("site_id") or payload.get("site_id"), 60)
    game = _text(session.get("session_id") or payload.get("game_session_id"), 80)
    if open_count:
        overall = f"🔴 장애 진행 중 {open_count}건"
    elif unrecovered:
        overall = "🟠 미복구 종료 포함"
    else:
        overall = "🟢 모두 복구"
    title = " · ".join(part for part in (overall, _session_label(game)) if part)
    opened = [stamp for stamp in (_parse_time(item.get("opened_utc")) for item in items) if stamp]
    states = ", ".join(f"{label} {count}" for label, count in (("진행 중", open_count), ("미복구", unrecovered), ("복구", recovered)) if count)
    # Same automatic rule as the Notion incident DB; SEV1 (game stopped, refund) is set by people.
    severity = "SEV2 미복구 있음" if unrecovered else "SEV3 자동 판정"
    # Which game: group, theme and when it ran, as the collector saw its snapshots (none outside games).
    started, ended = session.get("started_utc"), session.get("ended_utc")
    group, theme = _text(session.get("game_group"), 20), _text(session.get("theme"), 40)
    end = _kst_since(ended, started)
    period = _kst(started) and _kst(started) + " 시작" + (f" ~ {end} 종료" if end else "")
    which = [group and group.upper(), theme, period, site]
    first = _kst_since(min(opened).isoformat(), started) if opened else None
    summary = [f"장애 {total}건" + (f" ({states})" if states else ""), first and "첫 장애 " + first, severity]
    # The collector lists unresolved incidents first, then the newest.
    lines = [_session_item_line(item) for item in items[:SESSION_LINES]]
    if total > len(lines):
        lines.append(f"외 {total - len(lines)}건")
    link = _notion_link(session.get("notion_page"), "📒 노션 장애 페이지")
    blocks = [{"type": "header", "text": {"type": "plain_text", "text": title[:150]}},
              *[{"type": "context", "elements": [{"type": "plain_text", "text": " · ".join(part for part in parts if part)}]}
                for parts in (which, summary) if any(parts)],
              {"type": "divider"},
              {"type": "section", "text": {"type": "plain_text", "text": ("\n".join(lines) or "장치 정보 없음")[:2900]}},
              {"type": "context", "elements": ([link] if link else []) + [{"type": "plain_text", "text": "원인 미확정 · 장치별 경과는 스레드"}]}]
    # The notification preview names the store too: the title alone does not.
    return {"text": _fallback(" · ".join(part for part in (title, site) if part)), "blocks": blocks,
            "unfurl_links": False, "unfurl_media": False}


def slack_event_reply(payload, session):
    """One device event in the session card's thread."""
    status, device = payload.get("status"), _text(payload.get("device_id"), 60) or "알 수 없는 장치"
    try:
        gap = _duration(int(payload.get("gap_ms")) / 1000)
    except (ValueError, TypeError, OverflowError):
        gap = None
    if status == "offline":
        role = _text(payload.get("role"), 40)
        parts = [f"🔴 {device} 연결 끊김", _kst(payload.get("last_seen_utc")) and "마지막 수신 " + _kst(payload.get("last_seen_utc")),
                 gap and "무응답 " + gap, role and role != "unknown" and "역할 " + role]
    elif status == "recovered":
        outage = _outage(payload)
        parts = [f"🟢 {device} 복구", outage and outage + " 만에", _close_reason(payload)]
    elif status == "rebooted":
        firmware = _text(payload.get("firmware"), 20)
        parts = [f"🔁 {device} 재부팅", _reboot_reason(payload), firmware and "펌웨어 " + firmware,
                 _kst(payload.get("occurred_at")) and "확인 " + _kst(payload.get("occurred_at"))]
    else:
        parts = [f"⚪ {device} 미복구 종료", _close_reason(payload),
                 _kst(payload.get("opened_at")) and "끊긴 시각 " + _kst(payload.get("opened_at"))]
    text = " · ".join(part for part in parts if part)
    blocks = [{"type": "section", "text": {"type": "plain_text", "text": text}}]
    link = _notion_link(session.get("event_page"), "📒 노션 이벤트")
    if link:
        blocks.append({"type": "context", "elements": [link]})
    return {"text": _fallback(text), "blocks": blocks, "unfurl_links": False, "unfurl_media": False}


def _slack_api(call, token, method, body):
    # chat.update rewrites the same message, so only it is safe to retry blindly.
    safe = method == "chat.update"
    response = call("https://slack.com/api/" + method, "POST", body, token)
    failure = _classify(response, safe_retry=safe)
    if failure:
        return None, failure
    try:
        data = json.loads(response.get("body", ""))
        if not isinstance(data, dict):
            raise ValueError()
    except ValueError:
        return None, DeliveryResult("retryable" if safe else "unknown", "unexpected_response", 30)
    if data.get("ok") is not True:
        error = data.get("error")
        error = error if isinstance(error, str) and re.fullmatch(r"[a-z_]{1,48}", error) else "error"
        if error in SLACK_TRANSIENT:
            # A possibly-posted message must not be resent; an edit or an unprocessed request can be.
            retry = safe or error == "service_unavailable"
            return None, DeliveryResult("retryable" if retry else "unknown", "slack_" + error, 30)
        return None, DeliveryResult("permanent", "slack_" + error)
    return data, None


def _send_slack_session(call, token, channel_id, payload):
    """Edit (or open) the game session's card, and reply in its thread to what needs a person.

    Drops and recoveries live on the card, so the thread stays free for people working the incident.
    """
    session = payload.get("slack_session")
    if not isinstance(session, dict):
        # Without the collector's session context, the card covers this event alone.
        state = {"offline": "open", "recovered": "recovered", "rebooted": "recovered"}.get(payload.get("status"), "closed")
        session = {"incidents": [{"device_id": payload.get("device_id"), "state": state,
                                  "opened_utc": payload.get("opened_at") or payload.get("occurred_at"),
                                  "updated_utc": payload.get("occurred_at"),
                                  "reason": "generator_reboot" if payload.get("status") == "rebooted" else None}]}
    card = slack_session_card(payload, session)
    # A pre-upgrade follow-up still carries its per-device card: take it over as the session card.
    parent = next((ts for ts in (session.get("ts"), payload.get("slack_ts")) if isinstance(ts, str) and SLACK_TS.fullmatch(ts)), None)
    edited = False
    if parent:
        # Edit first: a failed edit retries without leaving a duplicate reply.
        _, failure = _slack_api(call, token, "chat.update", {"channel": channel_id, "ts": parent,
                                "text": card["text"], "blocks": card["blocks"]})
        if failure and failure.code not in SLACK_ORIGINAL_GONE:
            return failure
        if failure:
            parent = None  # Card deleted or no longer editable: open a new one for the session.
        else:
            edited = True
    if not parent:
        data, failure = _slack_api(call, token, "chat.postMessage", dict(card, channel=channel_id))
        if failure:
            return failure
        parent = data.get("ts")
        if not isinstance(parent, str) or not SLACK_TS.fullmatch(parent):
            return DeliveryResult("unknown", "invalid_write_response")
    if payload.get("refresh"):
        # A reboot noted on an announced outage: the edited line, plus a quiet note in the thread.
        reboot = payload.get("reboot")
        if not isinstance(reboot, dict):
            return DeliveryResult("delivered", "ok", remote_id=parent, parent_id=parent)
        payload = dict(payload, status="rebooted", device_id=reboot.get("name"), reset_label=reboot.get("label"),
                       firmware=reboot.get("firmware"), occurred_at=reboot.get("observed_utc"))
    open_count, recovered, unrecovered = _session_counts(session)
    # Edits notify nobody. Thread what needs a person: a device unrecovered at game end, a new drop on
    # a card that had settled (everything else had already ended), and a reboot (thread only).
    reopened = payload.get("status") == "offline" and edited and open_count == 1 and recovered + unrecovered > 0
    if payload.get("status") not in ("monitoring_ended", "rebooted") and not reopened:
        return DeliveryResult("delivered", "ok", remote_id=parent, parent_id=parent)
    # Replies stay in the thread, never also shown in the channel: the channel holds only the cards.
    reply = dict(slack_event_reply(payload, session), channel=channel_id, thread_ts=parent)
    _, failure = _slack_api(call, token, "chat.postMessage", reply)
    if failure:
        # The card exists either way; keep its ts so the retry threads under it.
        return DeliveryResult(failure.state, failure.code, failure.retry_after, parent, parent)
    return DeliveryResult("delivered", "ok", remote_id=parent, parent_id=parent)


def _send_slack_evidence(call, token, channel_id, payload):
    """Quiet thread reply: the device's Notion blackbox table is ready, with its highlights."""
    session = payload.get("slack_session") if isinstance(payload.get("slack_session"), dict) else {}
    card = session.get("ts")
    if not isinstance(card, str) or not SLACK_TS.fullmatch(card):
        return DeliveryResult("permanent", "no_session_card")
    device = _text(payload.get("device_id"), 60) or "알 수 없는 장치"
    highlights = [_text(item, 120) for item in payload.get("highlights") or [] if isinstance(item, str)][:4]
    text = " · ".join([f"📦 {device} 블랙박스 기록"] + highlights)
    blocks = [{"type": "section", "text": {"type": "plain_text", "text": text}}]
    link = _notion_link(session.get("event_page"), "📒 노션 블랙박스 표")
    if link:
        blocks.append({"type": "context", "elements": [link]})
    _, failure = _slack_api(call, token, "chat.postMessage", {"channel": channel_id, "thread_ts": card, "text": _fallback(text),
                            "blocks": blocks, "unfurl_links": False, "unfurl_media": False})
    return failure or DeliveryResult("delivered", "ok", remote_id=card)


def _rich(text):
    return [{"type": "text", "text": {"content": _text(text)}}]


def _select_name(value):
    # Notion select option names cannot contain commas; unknown names become new options.
    return _text(value, 100).replace(",", " ").strip()


def _iso(stamp):
    # Notion keeps date-times to the minute; sub-second digits only risk a rejected write.
    return stamp.isoformat(timespec="seconds") if stamp else None


def _notion_find(call, token, source_id, prop, value, *, unique):
    """Look up a page by a collector-owned text key. Reads are always safe to retry."""
    body = {"filter": {"property": prop, "rich_text": {"equals": value}}, "page_size": 2}
    if not unique:
        # Duplicated session pages are possible; every lookup must settle on the same one.
        body["page_size"] = 100
    response = call(f"https://api.notion.com/v1/data_sources/{source_id}/query", "POST", body, token)
    failure = _classify(response, safe_retry=True)
    if failure:
        return None, failure
    if response.get("oversized"):
        return None, DeliveryResult("permanent", "oversized_query_response")
    try:
        found = json.loads(response.get("body", ""))
        pages = found["results"]
        if not isinstance(pages, list):
            raise ValueError()
        if unique and (len(pages) > 1 or found.get("has_more")):
            return None, DeliveryResult("permanent", "duplicate_incident_records")
        if not pages:
            return None, None
        # created_time is minute-granular, so the page id breaks ties deterministically.
        oldest = min(pages, key=lambda page: (str(page.get("created_time", "")), _uuid(page["id"])))
        return _uuid(oldest["id"]), None
    except (ValueError, KeyError, TypeError):
        return None, DeliveryResult("retryable", "invalid_query_response", 30)


def _opened_at(payload, status):
    # Payloads queued before opened_at existed only carry the outage start on the offline event.
    return _parse_time(payload.get("opened_at") or (payload.get("occurred_at") if status == "offline" else None))


def _event_properties(payload, status, incident_id):
    """Device-event columns. Missing values are omitted so an update never clears them."""
    device = _select_name(payload.get("device_id")) or "알 수 없는 장치"
    rebooted = status == "rebooted"
    properties = {"이벤트": {"title": _rich(device + (" 재부팅" if rebooted else " 연결 끊김"))}, "사건 ID": {"rich_text": _rich(incident_id)},
                  "상태": {"status": {"name": NOTION_STATUS_NAMES[status]}},
                  "매장": {"select": {"name": _select_name(payload.get("site_id"))}},
                  "장치": {"select": {"name": device}}}
    role, mac, summary = _select_name(payload.get("role")), _text(payload.get("device_mac"), 40), _text(payload.get("summary"))
    opened = _opened_at(payload, status)
    if role:
        properties["역할"] = {"select": {"name": role}}
    if mac:
        properties["MAC"] = {"rich_text": _rich(mac)}
    if summary:
        properties["요약"] = {"rich_text": _rich(summary)}
    if opened:
        properties["발생 시각"] = {"date": {"start": _iso(opened)}}
    if status != "offline":
        ended = _parse_time(payload.get("occurred_at"))
        reason = _select_name("재부팅 " + _reboot_reason(payload) if rebooted else _close_reason(payload))
        if ended:
            properties["종료 시각"] = {"date": {"start": _iso(ended)}}
        if reason:
            properties["종료 사유"] = {"select": {"name": reason}}
        if opened and ended and not rebooted:
            # Notion stores date-times to the minute, so the duration cannot be derived from the dates there.
            seconds = round((ended - opened).total_seconds())
            if seconds >= 0:
                properties["지속(초)"] = {"number": seconds}
    return properties


def _notion_session(call, token, source_id, site, session, payload, cache):
    """Find or create the incident page for one game session (key: site/session)."""
    key = f"{site}/{session}"
    if (source_id, key) in cache:
        # Devices of one session often drop together; a query right after the create
        # may not see the new page yet, so reuse what this process already resolved.
        return cache[source_id, key], None
    page_id, failure = _notion_find(call, token, source_id, "세션 키", key, unique=False)
    if failure:
        return None, failure
    if not page_id:
        opened = _iso(_opened_at(payload, payload.get("status")))
        when, started, group = _kst(opened), _kst(payload.get("session_started_utc")), _text(payload.get("session_group"), 20)
        # A game is named by its group and start; a day outside games by its first incident.
        day = session.startswith("게임외-")  # its label already carries the date
        title = (site, _session_label(session), group and group.upper(), started[:11] + " 시작") if started else \
            (site, when and (when[6:11] if day else when[:11]), _session_label(session))
        properties = {"장애": {"title": _rich(" · ".join(part for part in title if part))},
                      "세션 키": {"rich_text": _rich(key)}, "매장": {"select": {"name": site}},
                      "게임 세션 ID": {"rich_text": _rich(session)}}
        if opened:
            properties["처음 발생"] = {"date": {"start": opened}}
        response = call("https://api.notion.com/v1/pages", "POST", {
            "parent": {"type": "data_source_id", "data_source_id": source_id}, "properties": properties}, token)
        # A retry looks the key up again before creating, so a lost response can at worst leave
        # one extra empty session page; it never duplicates or drops the device event itself.
        failure = _classify(response, safe_retry=True)
        if failure:
            return None, failure
        try:
            page_id = _uuid(json.loads(response.get("body", ""))["id"])
        except (ValueError, KeyError, TypeError):
            return None, DeliveryResult("retryable", "invalid_write_response", 30)
    cache[source_id, key] = page_id
    while len(cache) > NOTION_SESSION_CACHE:
        cache.pop(next(iter(cache)))
    return page_id, None


def _evidence_blocks(evidence):
    rows = evidence["rows"]
    if not isinstance(rows, list) or len(rows) > 98 or any(not isinstance(row, list) or len(row) != 3 for row in rows):
        raise ValueError("Invalid evidence rows")
    note = ["후보 기록이며 원인은 확정되지 않았습니다.",
            "장치 시각은 실제 시각이 아니라 부팅 회차와 켜진 뒤 경과 시간입니다.",
            f"수집 {_kst(evidence.get('collected_utc')) or '-'} · 원본은 매장 PC에서 "
            f"incidents.py --incident-id {_text(evidence.get('incident_id'), 160)}로 확인합니다."]
    if evidence.get("truncated"):
        note.append("일부만 표시했습니다.")
    if evidence.get("device_pending"):
        note.append("장치 기록이 아직 올라오지 않았습니다.")
    table = [["출처", "시각", "내용"]] + (rows or [["-", "-", "관련 기록 없음"]])
    return [{"type": "heading_3", "heading_3": {"rich_text": _rich(EVIDENCE_HEADING + " (후보 · 원인 미확정)")}},
            {"type": "paragraph", "paragraph": {"rich_text": _rich(" ".join(note))}},
            {"type": "table", "table": {"table_width": 3, "has_column_header": True, "has_row_header": False,
             "children": [{"type": "table_row", "table_row": {"cells": [_rich(_text(cell, 300) or "-") for cell in row]}}
                          for row in table]}}]


def _append_evidence(call, token, payload):
    """Append the blackbox table to the device-event page body exactly once."""
    page_id = _uuid(payload.get("notion_page_id"))
    blocks = _evidence_blocks(payload["evidence"])
    url = f"https://api.notion.com/v1/blocks/{page_id}/children"
    # Appending is not idempotent, so every attempt first looks for an earlier append.
    response = call(url, "GET", None, token)
    failure = _classify(response, safe_retry=True)
    if failure:
        return failure
    if response.get("oversized"):
        return DeliveryResult("permanent", "oversized_page_response")
    try:
        children = json.loads(response.get("body", ""))["results"]
        if not isinstance(children, list):
            raise ValueError()
        # Only the first 100 blocks are checked; the summary is the collector's first append.
        if any(block.get("type") == "heading_3" and "".join(part.get("plain_text", "") for part in
               block["heading_3"].get("rich_text", [])).startswith(EVIDENCE_HEADING) for block in children):
            return DeliveryResult("delivered", "already_attached", remote_id=page_id)
    except (ValueError, KeyError, TypeError, AttributeError):
        return DeliveryResult("retryable", "invalid_children_response", 30)
    response = call(url, "PATCH", {"children": blocks}, token)
    failure = _classify(response, safe_retry=True)
    return failure or DeliveryResult("delivered", "ok", remote_id=page_id)


class NotificationSender:
    def __init__(self, config, *, credential_reader=read_machine_password, transport=_transport):
        self.config = config
        self.credential_reader = credential_reader
        self.transport = transport
        self.notion_sessions = {}

    def send(self, channel, payload):
        if channel not in ("slack", "notion") or not self.config.get("enabled") or not self.config.get(channel, {}).get("enabled"):
            return DeliveryResult("permanent", "channel_disabled")
        try:
            credentials = json.loads(self.credential_reader(self.config["credential_file"]))
            if not isinstance(credentials, dict):
                raise ValueError()
            incident_id = payload.get("incident_id")
            if not isinstance(incident_id, str) or not re.fullmatch(r"[A-Za-z0-9_.:-]{1,160}", incident_id):
                return DeliveryResult("permanent", "invalid_incident_id")
            deadline = time.monotonic() + TOTAL_TIMEOUT
            def call(url, method, body, token=None):
                _allowed_url(url, method)
                # (t + 10) - t can round above 10 when the coarse Windows
                # monotonic clock has not ticked; never exceed the budget.
                remaining = min(TOTAL_TIMEOUT, deadline - time.monotonic())
                if remaining <= 0:
                    return {"network_error": True}
                return self.transport({"url": url, "method": method, "body": body, "token": token}, remaining)
            if channel == "slack":
                channel_id = self.config["slack"].get("channel_id")
                if channel_id:
                    token = credentials.get("slack_bot_token")
                    if not isinstance(token, str) or not SLACK_TOKEN.fullmatch(token):
                        return DeliveryResult("permanent", "invalid_credentials")
                    if payload.get("evidence_notice"):
                        return _send_slack_evidence(call, token, channel_id, payload)
                    return _send_slack_session(call, token, channel_id, payload)
                if payload.get("evidence_notice"):
                    # Queued while in bot mode; a webhook has no card to thread under.
                    return DeliveryResult("permanent", "webhook_mode_skips_evidence_notice")
                if payload.get("refresh"):
                    # A webhook message cannot be edited; resending would announce it again.
                    return DeliveryResult("permanent", "webhook_mode_skips_refresh")
                if payload.get("slack_ts") and payload.get("status") != "recovered":
                    # Queued while in bot mode; webhooks never announce monitoring_ended.
                    return DeliveryResult("permanent", "webhook_mode_skips_monitoring_end")
                url = credentials.get("slack_webhook_url")
                response = call(url, "POST", slack_message(payload))
                failure = _classify(response)
                if failure:
                    return failure
                return DeliveryResult("delivered", "ok") if response.get("body", "").strip() == "ok" else DeliveryResult("unknown", "unexpected_response")
            token = credentials.get("notion_token")
            if not isinstance(token, str) or not re.fullmatch(r"[A-Za-z0-9_-]{10,512}", token):
                return DeliveryResult("permanent", "invalid_credentials")
            if "evidence" in payload:
                if not isinstance(payload["evidence"], dict):
                    return DeliveryResult("permanent", "evidence_not_built")
                return _append_evidence(call, token, payload)
            source_id = _uuid(self.config["notion"]["data_source_id"])
            status = payload.get("status")
            if not isinstance(status, str) or status not in NOTION_STATUS_NAMES:
                return DeliveryResult("permanent", "unsupported_incident_status")
            site, session = _select_name(payload.get("site_id")), _text(payload.get("game_session_id"), 80)
            if not site or not session:
                return DeliveryResult("permanent", "missing_game_session")
            response = call(f"https://api.notion.com/v1/data_sources/{source_id}", "GET", None, token)
            session_source, failure = _notion_schema(response, source_id)
            if failure:
                return failure
            try:
                # The follow-up of a delivered event carries its page id: a query issued right
                # after that create may not see the page yet and would create a duplicate.
                page_id = _uuid(payload["notion_page_id"])
            except (KeyError, ValueError):
                page_id, failure = _notion_find(call, token, source_id, "사건 ID", incident_id, unique=True)
                if failure:
                    return failure
            properties = _event_properties(payload, status, incident_id)
            session_key = session_page = None
            if page_id:
                # The session relation was set when the page was created; updates leave it alone.
                response = call("https://api.notion.com/v1/pages/" + page_id, "PATCH", {"properties": properties}, token)
            else:
                response = call(f"https://api.notion.com/v1/data_sources/{session_source}", "GET", None, token)
                _, failure = _schema_properties(response, session_source, NOTION_SESSION_PROPERTIES)
                if failure:
                    return failure
                session_page, failure = _notion_session(call, token, session_source, site, session, payload, self.notion_sessions)
                if failure:
                    return failure
                if deadline - time.monotonic() < NOTION_CREATE_RESERVE:
                    # Nothing sent yet: retrying re-reads both keys, whereas a create cut off by
                    # the budget would be an unknown outcome needing manual review.
                    return DeliveryResult("retryable", "budget_exhausted", 30)
                session_key = (session_source, f"{site}/{session}")
                properties["게임 세션"] = {"relation": [{"id": session_page}]}
                response = call("https://api.notion.com/v1/pages", "POST", {
                    "parent": {"type": "data_source_id", "data_source_id": source_id}, "properties": properties}, token)
            failure = _classify(response, safe_retry=bool(page_id))
            if failure:
                # A cached session page may have been deleted by an operator; look it up again next time.
                self.notion_sessions.pop(session_key, None)
                return failure
            try:
                remote_id = _uuid(json.loads(response.get("body", ""))["id"])
            except (ValueError, KeyError, TypeError):
                return DeliveryResult("unknown", "invalid_write_response")
            return DeliveryResult("delivered", "ok", remote_id=remote_id, parent_id=session_page)
        except (ValueError, TypeError, KeyError, OSError, RuntimeError):
            # Never include exception text: it can contain a webhook URL or credential.
            return DeliveryResult("permanent", "configuration_or_credentials")


if __name__ == "__main__":
    mode = sys.argv[1:]
    if mode not in (["--http-worker"], ["--resolve-notion-database"], ["--check-notion-schema"]):
        raise SystemExit("Use the collector notification worker or configuration helper")
    try:
        # Windows PowerShell / .NET redirected stdin may prepend a UTF-8 BOM.
        # Decode bytes explicitly so the protocol also ignores the host locale.
        raw = sys.stdin.buffer.read(MAX_BODY + 1)
        if len(raw) > MAX_BODY:
            raise ValueError()
        request = json.loads(raw.decode("utf-8-sig"))
        if mode == ["--http-worker"]:
            result = _http_child(request)
        elif mode == ["--check-notion-schema"]:
            result = asdict(check_notion_schema(request.get("data_source_id"), request.get("token")))
        else:
            result = asdict(resolve_notion_database(request.get("database_url"), request.get("token")))
    except Exception:
        result = {"network_error": True}
    sys.stdout.write(json.dumps(result))
