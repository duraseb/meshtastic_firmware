#!/usr/bin/env python3
"""
Validate the AiWeatherSource Wikidata parsing logic used by the C++ implementation.

The parser is used in two forms:
  1) Buffered JSON parsing (full payload in memory).
  2) Streaming-style parsing that scans to the "results"/"bindings" path and
     then extracts objects incrementally.
"""

from __future__ import annotations

import argparse
import json
from typing import Tuple, List
from datetime import datetime
from urllib.parse import quote_plus
from urllib.request import Request, urlopen
from urllib.error import HTTPError, URLError
import ssl


WIKIDATA_MAX_PROMPT_CHARS = 3000
WIKIDATA_MAX_CANDIDATES = 100
WIKIDATA_QUERY_URL = "https://query.wikidata.org/sparql"


def _skip_ws(payload: str, index: int) -> int:
    """Advance index over ASCII whitespace."""
    while index < len(payload) and payload[index].isspace():
        index += 1
    return index


def _build_hints(bindings: List[dict], birth_date: str) -> Tuple[bool, str]:
    hints = "Lista kandydatów urodzonych " + birth_date + ":\n"
    added = 0
    for binding in bindings:
        if added >= WIKIDATA_MAX_CANDIDATES:
            break

        label = (
            binding.get("personLabel", {})
            .get("value")
            if isinstance(binding.get("personLabel", {}), dict)
            else None
        )
        description = (
            binding.get("personDescription", {})
            .get("value")
            if isinstance(binding.get("personDescription", {}), dict)
            else None
        )

        if not label:
            continue

        line = f"- {added + 1}. {label}"
        if description:
            line += f" — {description}"

        if len(hints) + len(line) > WIKIDATA_MAX_PROMPT_CHARS:
            break

        hints += line + "\n"
        added += 1

    if added == 0:
        return False, ""

    return True, hints


def parse_like_buffer(payload: str, birth_date: str):
    """Mimic existing buffered parser behavior."""
    try:
        doc = json.loads(payload)
    except json.JSONDecodeError as err:
        return False, f"Buffered JSON parse error: {err}"

    bindings = (
        doc.get("results", {})
        .get("bindings", [])
        if isinstance(doc, dict)
        else []
    )

    if not isinstance(bindings, list):
        return False, "Buffered parse error: results.bindings is not a list"
    if len(bindings) == 0:
        return False, "Buffered parse error: no bindings"

    ok, hints = _build_hints(bindings, birth_date)
    if not ok:
        return False, "Buffered parse error: no usable person entries"
    return True, hints


def parse_like_stream(payload: str, birth_date: str):
    """
    Mimic the C++ streaming parser logic:
      - locate "results"
      - locate "bindings"
      - require [array], then parse objects sequentially
    """
    results_idx = payload.find('"results"')
    if results_idx < 0:
        return False, "Streaming parse error: could not locate results"

    bindings_idx = payload.find('"bindings"', results_idx)
    if bindings_idx < 0:
        return False, "Streaming parse error: could not locate bindings"

    index = bindings_idx + len('"bindings"')
    index = _skip_ws(payload, index)
    if index >= len(payload):
        return False, "Streaming parse error: reached end while locating bindings value"

    if payload[index] == ':':
        index += 1
        index = _skip_ws(payload, index)
    else:
        return False, "Streaming parse error: missing ':' after bindings"

    if index >= len(payload) or payload[index] != '[':
        return False, "Streaming parse error: bindings value is not an array"

    index += 1  # skip '['
    index = _skip_ws(payload, index)
    if index >= len(payload):
        return False, "Streaming parse error: no bindings content"

    if payload[index] == ']':
        return False, "Streaming parse error: response had empty bindings array"

    bindings = []
    decoder = json.JSONDecoder()

    while index < len(payload):
        index = _skip_ws(payload, index)
        if index >= len(payload):
            break
        if payload[index] == ']':
            break
        if payload[index] == ',':
            index += 1
            continue

        try:
            value, consumed = decoder.raw_decode(payload[index:])
        except json.JSONDecodeError as err:
            return False, f"Streaming parse error: object decode failed at {index}: {err}"

        if not isinstance(value, dict):
            return False, f"Streaming parse error: non-object binding at {index}"

        bindings.append(value)
        index += consumed

    if len(bindings) == 0:
        return False, "Streaming parse error: no binding objects parsed"

    ok, hints = _build_hints(bindings, birth_date)
    if not ok:
        return False, "Streaming parse error: no usable person entries"
    return True, hints


def run_case(name: str, payload: str, birth_date: str) -> None:
    print(f"\n=== {name} ===")
    buf_ok, buf_val = parse_like_buffer(payload, birth_date)
    str_ok, str_val = parse_like_stream(payload, birth_date)

    print(f"buffer_ok={buf_ok}")
    if buf_ok:
        print(f"buffer_hints_len={len(buf_val)}")
        print(buf_val.splitlines()[:3])
    else:
        print(f"buffer_error={buf_val}")

    print(f"stream_ok={str_ok}")
    if str_ok:
        print(f"stream_hints_len={len(str_val)}")
        print(str_val.splitlines()[:3])
    else:
        print(f"stream_error={str_val}")

    if buf_ok == str_ok and buf_val == str_val:
        print("match=true")
    else:
        print("match=false")


def sample_payload():
    return json.dumps(
        {
            "head": {
                "vars": ["personLabel", "personDescription"]
            },
            "results": {
                "bindings": [
                    {
                        "personLabel": {"type": "literal", "value": "Jan Kowalski"},
                        "personDescription": {"type": "literal", "value": "kompozytor"}
                    },
                    {
                        "personLabel": {"type": "literal", "value": "Maria Nowak"},
                        "personDescription": {"type": "literal", "value": "pisarka"}
                    }
                ]
            }
        },
        ensure_ascii=False,
        indent=2,
    )


def sample_payload_minified():
    return json.dumps(
        {
            "head": {"vars": ["personLabel", "personDescription"]},
            "results": {
                "bindings": [
                    {
                        "personLabel": {"type": "literal", "value": "Jan Kowalski"},
                        "personDescription": {"type": "literal", "value": "kompozytor"}
                    }
                ]
            }
        },
        ensure_ascii=False,
        separators=(",", ":"),
    )


def malformed_payload_no_bindings():
    return '{"head":{"vars":[]}, "results":{"foo":[]}}'


def malformed_payload_html():
    return "<html><body>blocked</body></html>"


def build_wikidata_birthday_query(month: int, day: int) -> str:
    return (
        "SELECT DISTINCT ?personLabel ?personDescription\n"
        "WHERE {\n"
        "  ?person wdt:P31 wd:Q5 .\n"
        "  ?person p:P569/psv:P569 [wikibase:timePrecision 11; wikibase:timeValue ?dob] .\n"
        f"  FILTER(MONTH(?dob) = {month} && DAY(?dob) = {day}) .\n"
        "  ?person wdt:P19 ?birthplace .\n"
        "  ?birthplace wdt:P131* wd:Q36 .\n"
        "  VALUES ?culturalOccupation {\n"
        "    wd:Q483501 wd:Q36180 wd:Q49757 wd:Q33999 wd:Q639669\n"
        "    wd:Q1028181 wd:Q177220 wd:Q214917 wd:Q10737 wd:Q1281618\n"
        "  }\n"
        "  ?person wdt:P106 ?occ .\n"
        "  ?occ wdt:P279* ?culturalOccupation .\n"
        "  ?person rdfs:label ?personLabel .\n"
        '  FILTER(LANG(?personLabel) = "pl")\n'
        '  SERVICE wikibase:label { bd:serviceParam wikibase:language "pl,en". }\n'
        "}\n"
        "ORDER BY DESC(?dob)\n"
        "LIMIT 30\n"
    )


def build_wikidata_url(month: int, day: int) -> str:
    query = build_wikidata_birthday_query(month, day)
    encoded_query = quote_plus(query, safe="-_.~")
    return f"{WIKIDATA_QUERY_URL}?query={encoded_query}&format=json"


def fetch_wikidata_payload(month: int, day: int, timeout_seconds: int = 65) -> Tuple[int, str, str]:
    url = build_wikidata_url(month, day)
    request = Request(url)
    request.add_header("User-Agent", "Mozilla/5.0 (PC; Meshtastic Parser Validation)")
    request.add_header("Accept", "application/sparql-results+json")
    request.add_header("Accept-Encoding", "identity")

    try:
        with urlopen(request, timeout=timeout_seconds, context=ssl._create_unverified_context()) as response:
            payload = response.read().decode("utf-8", errors="ignore")
            return response.getcode(), response.headers.get("Content-Type", ""), payload
    except HTTPError as e:
        error_payload = e.read().decode("utf-8", errors="ignore") if e.fp else ""
        return e.code, "error", error_payload
    except URLError as e:
        return -1, "error", f"{type(e).__name__}: {e}"
    except Exception as e:
        return -2, "error", f"{type(e).__name__}: {e}"


def infer_birth_date_label(month: int, day: int) -> str:
    months = (
        "stycznia", "lutego", "marca", "kwietnia", "maja", "czerwca",
        "lipca", "sierpnia", "września", "października", "listopada", "grudnia"
    )
    return f"{day} {months[month - 1]}"


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Validate streaming-like vs buffered Wikidata parser behavior"
    )
    parser.add_argument(
        "--payload-file",
        help="Optional path to JSON payload to validate against",
        default="",
    )
    parser.add_argument(
        "--birth-date",
        help="Birth date label used in prompts",
        default="1 stycznia",
    )
    parser.add_argument(
        "--self-test",
        help="Run built-in fixtures",
        action="store_true"
    )
    parser.add_argument(
        "--fetch",
        help="Fetch live data from Wikidata and validate parser behavior",
        action="store_true"
    )
    parser.add_argument(
        "--month",
        type=int,
        help="Month number for live fetch (1-12)",
        default=0
    )
    parser.add_argument(
        "--day",
        type=int,
        help="Day number for live fetch (1-31)",
        default=0
    )
    parser.add_argument(
        "--url",
        help="Optional explicit URL to fetch and validate",
        default=""
    )
    parser.add_argument(
        "--timeout",
        type=int,
        help="HTTP timeout seconds when fetching live data",
        default=65
    )
    args = parser.parse_args()

    if args.payload_file:
        with open(args.payload_file, "r", encoding="utf-8") as payload_handle:
            payload = payload_handle.read()
        run_case(args.payload_file, payload, args.birth_date)
        return

    if args.fetch or args.url:
        if args.url:
            request = Request(args.url)
            request.add_header("User-Agent", "Mozilla/5.0 (PC; Meshtastic Parser Validation)")
            request.add_header("Accept", "application/sparql-results+json")
            request.add_header("Accept-Encoding", "identity")
            try:
                with urlopen(request, timeout=args.timeout, context=ssl._create_unverified_context()) as response:
                    payload = response.read().decode("utf-8", errors="ignore")
                    print(
                        f"fetched_status={response.getcode()} "
                        f"content_type={response.headers.get('Content-Type', '')} "
                        f"bytes={len(payload)}"
                    )
                    run_case("fetched_url", payload, args.birth_date)
            except HTTPError as e:
                error_payload = e.read().decode("utf-8", errors="ignore") if e.fp else ""
                print(f"http_error={e.code} bytes={len(error_payload)}")
                print(f"error_payload_preview={error_payload[:180]}")
            except URLError as e:
                print(f"fetch_error=URLError:{e}")
            return

        month = args.month
        day = args.day
        if month == 0 or day == 0:
            today = datetime.utcnow()
            month = today.month
            day = today.day
            print(f"fetch_defaults: month/day not provided, using UTC now {month:02d}-{day:02d}")

        status, content_type, payload = fetch_wikidata_payload(month, day, args.timeout)
        if status < 0:
            print(f"fetch_error={payload}")
            return

        print(f"fetched_status={status} content_type={content_type} bytes={len(payload)}")
        print(f"fetched_url={build_wikidata_url(month, day)}")
        print(f"fetched_preview={payload[:180]}")
        run_case(
            f"live_{month:02d}_{day:02d}",
            payload,
            infer_birth_date_label(month, day)
        )
        return

    if args.self_test or not args.payload_file:
        run_case("formatted_json", sample_payload(), args.birth_date)
        run_case("minified_json", sample_payload_minified(), args.birth_date)
        run_case("no_bindings", malformed_payload_no_bindings(), args.birth_date)
        run_case("html_block", malformed_payload_html(), args.birth_date)


if __name__ == "__main__":
    main()
