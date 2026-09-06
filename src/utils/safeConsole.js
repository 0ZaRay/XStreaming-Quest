const REDACTED = '[REDACTED]';
const INSTALLED_FLAG = '__xstreamingSafeConsoleInstalled';

const normalizeKey = key =>
  String(key)
    .replace(/[^a-z0-9]/gi, '')
    .toLowerCase();

const shouldRedactKey = key => {
  const normalized = normalizeKey(key);
  if (!normalized) {
    return false;
  }

  return (
    normalized.includes('token') ||
    normalized.includes('authorization') ||
    normalized.includes('password') ||
    normalized.includes('credential') ||
    normalized.includes('clientsecret') ||
    normalized.includes('sharedsecret') ||
    normalized.includes('keymaterial') ||
    normalized.includes('proofkey') ||
    normalized.includes('cookie') ||
    normalized === 'secret' ||
    normalized === 'sessionkey' ||
    normalized === 'sessionid' ||
    normalized === 'devicecode' ||
    normalized === 'usercode' ||
    normalized === 'codeverifier' ||
    normalized === 'nonce' ||
    normalized === 'xuid' ||
    normalized === 'userhash' ||
    normalized === 'uhs' ||
    normalized === 'gamertag' ||
    normalized === 'email' ||
    normalized === 'consoleid' ||
    normalized === 'userid' ||
    normalized === 'deviceid' ||
    normalized === 'mgt' ||
    normalized === 'gtg' ||
    normalized === 'xid' ||
    normalized === 'exchangeresponse' ||
    normalized === 'sdp' ||
    normalized.endsWith('sdp')
  );
};

const redactString = (value, depth = 0) => {
  let text = String(value);
  const trimmed = text.trim();

  // A lot of existing XStreaming diagnostics call JSON.stringify before
  // logging. Parse those strings when possible so nested secrets can still be
  // removed without throwing away the surrounding diagnostic structure.
  if (
    depth < 4 &&
    trimmed.length > 1 &&
    ((trimmed.startsWith('{') && trimmed.endsWith('}')) ||
      (trimmed.startsWith('[') && trimmed.endsWith(']')))
  ) {
    try {
      const parsed = JSON.parse(trimmed);
      return JSON.stringify(redactLogValue(parsed, depth + 1));
    } catch (_) {
      // Not valid standalone JSON. Continue with textual redaction below.
    }
  }

  text = text.replace(
    /((?:X-)?Authorization\s*:\s*)[^\r\n]+/gi,
    '$1[REDACTED]',
  );
  text = text.replace(
    /((?:Set-)?Cookie\s*:\s*)[^\r\n]+/gi,
    '$1[REDACTED]',
  );
  text = text.replace(
    /Bearer\s+[A-Za-z0-9._~+/=-]+/gi,
    'Bearer [REDACTED]',
  );
  text = text.replace(
    /([?&](?:access_token|refresh_token|id_token|token|code|client_secret)=)[^&\s]+/gi,
    '$1[REDACTED]',
  );
  text = text.replace(
    /\beyJ[A-Za-z0-9_-]{16,}\.[A-Za-z0-9_-]{8,}(?:\.[A-Za-z0-9_-]{8,})?\b/g,
    '[REDACTED_JWT]',
  );

  // Preserve ICE/candidate diagnostics where useful, but strip the ephemeral
  // credentials and any SDES inline key material.
  text = text.replace(/(a=ice-pwd:)[^\r\n]+/gi, '$1[REDACTED]');
  text = text.replace(/(a=ice-ufrag:)[^\r\n]+/gi, '$1[REDACTED]');
  text = text.replace(
    /(a=crypto:[^\r\n]*inline:)[A-Za-z0-9+/=]+/gi,
    '$1[REDACTED]',
  );

  text = text.replace(
    /((?:["']?(?:access_token|refresh_token|id_token|authorization|authorizationtoken|gstoken|webtoken|xcloudtoken|xhometoken|credential|server_credential|password|client_secret|secret|sessionid|device_code|user_code|code_verifier)["']?)\s*[=:]\s*["']?)([^"',\s;&}\]]+)/gi,
    '$1[REDACTED]',
  );

  // SDP exchange payloads contain ephemeral session material. If a logging
  // transport already flattened the object into text, retain the useful label
  // while dropping the payload rather than risking a partial redaction.
  text = text.replace(
    /((?:sendChatSdp\.)?exchangeResponse\s*[:=]\s*).*/i,
    '$1[REDACTED]',
  );
  text = text.replace(/(\bsdp\s*[:=]\s*).*/i, '$1[REDACTED]');

  return text;
};

function redactLogValue(value, depth = 0, seen = new WeakSet()) {
  if (
    value == null ||
    typeof value === 'number' ||
    typeof value === 'boolean'
  ) {
    return value;
  }
  if (typeof value === 'string') {
    return redactString(value, depth);
  }
  if (typeof value === 'bigint') {
    return value.toString();
  }
  if (typeof value === 'function') {
    return `[Function ${value.name || 'anonymous'}]`;
  }
  if (depth >= 7) {
    return '[MaxDepth]';
  }
  if (value instanceof Date) {
    return value.toISOString();
  }
  if (value instanceof Error) {
    return {
      name: value.name,
      message: redactString(value.message || '', depth + 1),
      stack: redactString(value.stack || '', depth + 1),
    };
  }
  if (
    typeof ArrayBuffer !== 'undefined' &&
    ArrayBuffer.isView &&
    ArrayBuffer.isView(value)
  ) {
    return `[${value.constructor?.name || 'TypedArray'} ${
      value.byteLength || 0
    } bytes]`;
  }
  if (typeof value !== 'object') {
    return value;
  }
  if (seen.has(value)) {
    return '[Circular]';
  }

  seen.add(value);
  try {
    if (Array.isArray(value)) {
      return value.map(item => redactLogValue(item, depth + 1, seen));
    }

    const output = {};
    Object.keys(value).forEach(key => {
      let nextValue;
      try {
        nextValue = value[key];
      } catch (_) {
        nextValue = '[Unreadable]';
      }

      output[key] = shouldRedactKey(key)
        ? REDACTED
        : redactLogValue(nextValue, depth + 1, seen);
    });
    return output;
  } catch (_) {
    return '[Unserializable Object]';
  } finally {
    seen.delete(value);
  }
}

const redactLogArgs = args => args.map(arg => redactLogValue(arg));

const installSafeConsole = () => {
  const consoleAny = console;
  if (consoleAny[INSTALLED_FLAG]) {
    return;
  }

  try {
    Object.defineProperty(consoleAny, INSTALLED_FLAG, {
      value: true,
      configurable: false,
      enumerable: false,
      writable: false,
    });
  } catch (_) {
    consoleAny[INSTALLED_FLAG] = true;
  }

  ['log', 'info', 'warn', 'error', 'debug'].forEach(method => {
    const original = consoleAny[method];
    if (typeof original !== 'function') {
      return;
    }

    const boundOriginal = original.bind(consoleAny);
    consoleAny[method] = (...args) => boundOriginal(...redactLogArgs(args));
  });
};

module.exports = {
  redactLogValue,
  redactLogArgs,
  installSafeConsole,
};
