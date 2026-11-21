import argparse
from collections import defaultdict
from datetime import date
from html import unescape
import json
from pathlib import Path
import re
import unicodedata
from urllib.parse import urlsplit

from markdown_it import MarkdownIt


ROOT = Path(__file__).resolve().parents[1]
MARKDOWN = MarkdownIt("commonmark").enable("table")
SUFFIXES = {".md", ".mdx", ".txt", ".ts", ".tsx", ".js", ".jsx", ".py", ".html", ".json", ".csv", ".css", ".svg",
            ".yaml", ".yml", ".j2", ".jinja", ".jinja2", ".tmpl"}
REPORT_MODULES = ("backend/clinical.py", "backend/platform_db.py", "backend/research_reports.py")
METRIC = (r"(?<![a-z0-9_])(?:Dice|IoU|AUROC|AUC|MAE|RMSE|R²|F1|macro[- ]F1|accuracy|"
          r"sensitivity|specificity|recall|precision|Pearson)(?![a-z_])"
          r"|准确率|准确度|敏感度|灵敏度|特异度|召回率|精确率")
NUMBER = r"[+−-]?(?:\d+(?:\.\d+)?|\.\d+)(?:\s*%)?(?![\d.])"
METRIC_NAME = re.compile(METRIC, re.I)
METRIC_VALUE = re.compile(
    rf"(?:{METRIC})(?:\s+(?:score|of|was|is|from))?\s*(?:[:：=为约]|达到?|提升至|升至|从)?\s*(?P<after>{NUMBER})"
    rf"|(?<![a-z0-9.])(?P<before>{NUMBER})\s*(?:{METRIC})", re.I,
)
THIRD_PARTY = re.compile(r"第三方|third[- ]party", re.I)
DIRECTIONAL = re.compile(r"方向性|direction(?:al|\s+only)", re.I)
FIVE_POSITIVES = re.compile(r"(?<!\d)5(?:\s*(?:例|名)?\s*阳性|\s+positive(?:s| cases)?)", re.I)
RELEASE_REPO = "https://github.com/songconmaisaix31-design/echo-edge-training/releases/download/"


def scan_paths(root):
    names = (root / name for name in ("README.md", "ROADMAP.md", *REPORT_MODULES))
    for path in names:
        if path.is_file():
            yield path
    for folder in ("frontend/src", "docs", "templates", "report_templates", "backend/templates"):
        yield from sorted(path for path in (root / folder).rglob("*")
                          if path.is_file() and path.suffix.lower() in SUFFIXES)


def compact(text):
    return re.sub(r"\s+", "", unicodedata.normalize("NFKC", text)).casefold()


def visible(text):
    return re.sub(r"</?[a-z][^>]*>", " ", unescape(text), flags=re.I).replace("**", "").replace("`", "")


def metric_numbers(text):
    return {match.group("after") or match.group("before") for match in METRIC_VALUE.finditer(visible(text))}


def table_rows(tokens):
    section = None
    for token in tokens:
        if token.type in {"thead_open", "tbody_open"}:
            section = token.type
        elif token.type == "tr_open":
            line, cells = token.map[0], []
        elif token.type == "inline":
            cells.append(token.content)
        elif token.type == "tr_close":
            yield section, line, cells


def table_claims(tokens):
    start = None
    for index, token in enumerate(tokens):
        if token.type == "table_open":
            start = index
        if token.type != "table_close":
            continue
        headers = []
        for section, line, cells in table_rows(tokens[start:index]):
            if section == "thead_open":
                headers = cells
                yield line, tokens[start].map, {}, headers
                continue
            row_metric = bool(cells and METRIC_NAME.search(cells[0]))
            columns = {}
            for col, (header, cell) in enumerate(zip(headers, cells, strict=True)):
                values = metric_numbers(cell)
                if (METRIC_NAME.search(header) or (row_metric and col > 0)):
                    values.update(match.group() for match in re.finditer(NUMBER, visible(cell)))
                if values:
                    columns[col] = values
            if columns:
                yield line, tokens[start].map, columns, headers


def audited_rules(root):
    data = json.loads((root / "rules/metrics.json").read_text(encoding="utf-8"))
    reports = data["reports"]
    for key, report in reports.items():
        url = urlsplit(report["report"])
        if url.scheme != "https" or not url.netloc:
            raise ValueError(f"{key} 的来源须为 HTTPS")
        if report["kind"] == "project":
            prefix = f'{RELEASE_REPO}{report["release"]}/'
            if not report["release"] or not report["report"].startswith(prefix) or not url.path.endswith(".json"):
                raise ValueError(f"{key} 缺少对应 Release 的评估报告路径")
        elif report["kind"] != "third_party":
            raise ValueError(f"{key} 的来源类型无效")
    claims = {}
    for claim in data["claims"]:
        key = claim["path"], claim["text"]
        evidence = [reports[name] for name in claim["reports"]]
        if key in claims or not evidence or len({item["kind"] for item in evidence}) != 1:
            raise ValueError(f"指标登记项重复、无来源或混合来源 {key[0]}")
        headers = claim.get("headers")
        if headers is not None and (not isinstance(headers, list) or not all(isinstance(h, str) for h in headers)):
            raise ValueError(f"指标表头无效 {key[0]}")
        claims[key] = evidence[0]["kind"], headers
    exceptions = {}
    for item in data["exceptions"]:
        key = item["path"], item["text"], item["rule"]
        if key in exceptions or not item["reason"].strip() or item["rule"] not in {"banned_claim", "unregistered_metric"}:
            raise ValueError(f"例外项重复或缺少理由 {key[0]}")
        expires = date.fromisoformat(item["expires"])
        if expires <= date.today():
            raise ValueError(f"例外已到期 {key[0]} {expires}")
        exceptions[key] = item["reason"]
    banned = [phrase.strip() for phrase in (root / "rules/banned-claims.txt").read_text(encoding="utf-8").splitlines()
              if phrase.strip() and not phrase.lstrip().startswith("#")]
    if not banned:
        raise ValueError("禁词清单为空")
    return banned, claims, exceptions


def scan(root):
    banned, registered, exceptions = audited_rules(root)
    found = []
    for path in scan_paths(root):
        name = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8-sig")
        lines = text.splitlines()
        table_metrics = {}
        table_headers = {}
        sources = defaultdict(set)
        tokens = MARKDOWN.parse(text) if path.suffix in {".md", ".mdx"} else ()
        for line, (start, _end), columns, headers in table_claims(tokens):
            table_metrics[line] = columns
            table_headers[line] = headers
            kind, registered_headers = registered.get((name, lines[line].strip()), (None, None))
            if registered_headers != headers:
                kind = None
            for col in columns:
                if kind == "third_party" and not THIRD_PARTY.search(lines[line] + headers[col]):
                    found.append((name, line + 1, "third_party_label", lines[line].strip()))
                sources[start, col].add(kind or "project")
                if len(sources[start, col]) > 1:
                    found.append((name, line + 1, "mixed_metric_sources", lines[line].strip()))
        for line, raw in enumerate(lines):
            claim = raw.strip()
            key = name, claim
            public_text = compact(visible(claim))
            detected = [phrase for phrase in banned if compact(phrase) in public_text]
            if detected and (*key, "banned_claim") not in exceptions:
                found.append((name, line + 1, "banned_claim", "、".join(detected)))
            values = set() if line in table_metrics else metric_numbers(claim)
            for cell in table_metrics.get(line, {}).values():
                values.update(cell)
            kind, headers = registered.get(key, (None, None))
            if not values:
                if kind and headers != table_headers.get(line):
                    found.append((name, line + 1, "unregistered_metric", claim))
                continue
            if headers != table_headers.get(line):
                kind = None
            if kind is None and (*key, "unregistered_metric") not in exceptions:
                found.append((name, line + 1, "unregistered_metric", claim))
            if kind == "third_party" and line not in table_metrics and not THIRD_PARTY.search(claim):
                found.append((name, line + 1, "third_party_label", claim))
            if any(value.rstrip("0") in {"0.996", ".996"} for value in values) and (
                "ef" in visible(claim).casefold() or "ef" in name.casefold()
            ) and not (
                DIRECTIONAL.search(claim) and FIVE_POSITIVES.search(claim)
            ):
                found.append((name, line + 1, "ef_directional_only", claim))
        for token in tokens:
            if token.type != "inline" or token.map is None or token.map[1] - token.map[0] < 2:
                continue
            line, end = token.map
            pieces = lines[line:end]
            claim = " ".join(piece.strip() for piece in pieces)
            key = name, claim
            if metric_numbers(claim).difference(*(metric_numbers(piece) for piece in pieces)) and (
                key not in registered and (*key, "unregistered_metric") not in exceptions
            ):
                found.append((name, line + 1, "unregistered_metric", claim))
            detected = [phrase for phrase in banned if compact(phrase) in compact(visible(claim))
                        and not any(compact(phrase) in compact(visible(piece)) for piece in pieces)]
            if detected and (*key, "banned_claim") not in exceptions:
                found.append((name, line + 1, "banned_claim", "、".join(detected)))
    return found


def main():
    parser = argparse.ArgumentParser(description="核对公开文本的禁词与评估指标来源")
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()
    try:
        issues = scan(args.root.resolve())
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"红线规则无效 {error}")
        return 1
    for path, line, rule, detail in sorted(set(issues)):
        print(f"{path}:{line}: {rule} {detail}")
    if issues:
        return 1
    print("公开文本红线扫描通过")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
