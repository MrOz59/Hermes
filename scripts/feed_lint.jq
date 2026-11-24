def trim: gsub("^\\s+|\\s+$"; "");
def slug: test("^[a-z0-9][a-z0-9-]{0,63}$");
def levels: ["danger", "warning", "info", "success"];
def channels: ["all", "stable", "nightly"];
def document_keys: ["$schema", "version", "messages"];
def message_keys:
  ["id", "title", "body", "level", "channel", "affects",
   "link", "starts", "expires", "dismissible", "revision"];

def timestamp:
  if type != "string" then null
  else
    trim |
    [capture("^(?<year>[0-9]{4})-(?<month>[0-9]{2})-(?<day>[0-9]{2})(?:[Tt ](?<hour>[0-9]{2}):(?<minute>[0-9]{2})(?::(?<second>[0-9]{2})(?<fraction>\\.[0-9]+)?)?(?:[Zz]|(?<sign>[+-])(?<tzHour>[0-9]{2})(?::?(?<tzMinute>[0-9]{2}))?)?)?$")][0] |
    if . == null then null
    else
      .sign as $sign |
      .tzMinute as $zone_minutes |
      ("0" + (.fraction // "")) as $fraction |
      del(.sign, .fraction) | map_values((. // "0") | tonumber) |
      (.year % 4 == 0 and (.year % 100 != 0 or .year % 400 == 0)) as $leap |
      [31, (if $leap then 29 else 28 end), 31, 30, 31, 30, 31, 31, 30, 31, 30, 31] as $month_days |
      if .year < 1 or .month < 1 or .month > 12 or .day < 1 or .day > $month_days[.month - 1] or
         .hour > 24 or (.hour == 24 and (.minute + .second + ($fraction | tonumber) != 0)) or
         .minute > 59 or .second > 59 or .tzHour > 23 or .tzMinute > 59 or
         ($sign != null and $zone_minutes == null) then null
      else
        ([.year, .month - 1, .day, .hour, .minute, .second, 0, 0] | mktime) + ($fraction | tonumber) -
        ((.tzHour * 60 + .tzMinute) * (if $sign == "-" then -60 else 60 end))
      end
    end
  end;

def link_errors:
  . as $value |
  (if type == "object" then .url else . end) as $url |
  (if type == "object" then
     (if (keys - ["url", "label"] | length) > 0 then "link accepts only 'url' and 'label'" else empty end),
     (if .label != null and (.label | type) != "string" then "link.label must be a string" else empty end)
   else empty end),
  (if ($url | type) != "string" then
     "link must be a URL string or an object with a 'url'"
   elif ($url | trim | test("^https?://[^/?#]*"; "i") | not) or
     ($url | trim |
      [capture("^https?://(?<authority>[^/?#]*)"; "i")][0].authority |
      split("@") | last |
      test("^(\\[[0-9A-Fa-f:.]+\\]|[^\\s\\\\<>:%^|\\[\\]]+)(?::[0-9]*)?$") | not) then
     "link must be an absolute http(s) URL, not \($url | @json) (the client discards it)"
   else empty end);

def range_errors:
  (if type == "array" then . else [.] end) as $ranges |
  if ($ranges | length) == 0 then "affects must not be an empty list"
  else
    $ranges[] |
    if type != "string" then "each affects range must be a string"
    elif trim == "*" then empty
    elif trim == "" then "affects range is empty"
    else
      scan("\\S+") | select(test("^(>=|<=|>|<|=)?v?[0-9]+\\.[0-9]+\\.[0-9]+$") | not) |
      "\(tojson) is not a valid range clause (expected something like '>=0.5.0', '<0.5.2' or '0.5.1')"
    end
  end;

def add($where; $error): .errors += ["\($where): \($error)"];
def message_errors($message; $where):
  ($message.starts | timestamp) as $starts |
  ($message.expires | timestamp) as $expires |
  [
    (if ($message.title | type) != "string" or ($message.title | trim) == "" then
       "title is required and must be a non-empty string" else empty end),
    (if ($message | has("body")) and ($message.body | type) != "string" then "body must be a string" else empty end),
    (if ($message | has("level")) and (($message.level | type) != "string" or (levels | index($message.level)) == null) then
       "level must be one of \(levels | join(", "))" else empty end),
    (if ($message | has("channel")) and (($message.channel | type) != "string" or (channels | index($message.channel)) == null) then
       "channel must be one of \(channels | join(", "))" else empty end),
    (if ($message | has("dismissible")) and ($message.dismissible | type) != "boolean" then
       "dismissible must be true or false" else empty end),
    (if ($message | has("revision")) and
        (($message.revision | type) != "number" or $message.revision < 1 or ($message.revision | floor) != $message.revision) then
       "revision must be an integer >= 1" else empty end),
    (if ($message | has("affects")) then $message.affects | range_errors else empty end),
    (if ($message | has("link")) then $message.link | link_errors else empty end),
    (if ($message | has("starts")) and $starts == null then "starts must be an ISO 8601 date string" else empty end),
    (if ($message | has("expires")) and $expires == null then "expires must be an ISO 8601 date string" else empty end),
    (if $starts != null and $expires != null and $starts >= $expires then
       "expires is not after starts, so the message would never appear" else empty end)
  ] as $issues |
  reduce $issues[] as $issue (. ; add($where; $issue));

def lint_message($entry):
  "messages[\($entry.key)]" as $where |
  $entry.value as $message |
  if ($message | type) != "object" then add($where; "must be an object")
  else
    ($message | keys - message_keys) as $unknown |
    (if ($unknown | length) > 0 then
       add($where; "unknown field(s) \($unknown | join(", ")) (the client ignores them)")
     else . end) |
    (if ($message.id | type) == "string" then $message.id | trim else "" end) as $id |
    if $id == "" then
      add($where; "id is required and must be a non-empty string") | message_errors($message; $where)
    else
      "\($where) (\($id))" as $where |
      (if ($id | slug | not) then add($where; "id must be a lowercase slug, e.g. 'kms-cursor-rdna3'") else . end) |
      (if .seen[$id] then add($where; "duplicate id — only the first entry is shown") else . end) |
      .seen[$id] = true | message_errors($message; $where)
    end
  end;

if length != 1 then error("expected exactly one JSON document") else .[0] end |
. as $document |
{errors: [], seen: {}, count: 0} |
(if ($document | type) == "object" then
   ($document | keys - document_keys) as $unknown |
   (if ($unknown | length) > 0 then add("document"; "unknown field(s) \($unknown | join(", "))") else . end) |
   (if ($document.messages | type) != "array" then add("document"; "'messages' is required and must be a list") else . end)
 elif ($document | type) == "array" then .
 else add("document"; "the feed must be an object with a 'messages' list") end) |
(if ($document | type) == "array" then $document
 elif ($document | type) == "object" and ($document.messages | type) == "array" then $document.messages
 else [] end) as $messages |
.count = ($messages | length) |
reduce ($messages | to_entries[]) as $entry (. ; lint_message($entry)) |
del(.seen)
