import { HttpParams, HttpResult, RelayError } from "./common";

/** Runs one device-requested HTTP call. fetchFn is injected so tests and
    the offline path stay deterministic. */
export async function executeHttpRequest(options: {
  params: HttpParams;
  bodyBytes?: Uint8Array;
  fetchFn: typeof fetch;
}): Promise<HttpResult | { error: RelayError }> {
  const { params, bodyBytes, fetchFn } = options;
  let body: BodyInit | undefined;
  if (bodyBytes !== undefined) {
    body = bodyBytes as unknown as BodyInit;
  } else if (params.body !== undefined) {
    body = JSON.stringify(params.body);
  }
  try {
    const response = await fetchFn(params.url, {
      method: params.method,
      headers: {
        ...(bodyBytes === undefined && params.body !== undefined
          ? { "content-type": "application/json" }
          : {}),
        ...(bodyBytes !== undefined
          ? { "content-type": "application/octet-stream" }
          : {}),
        ...params.headers,
      },
      body,
    });
    const text = await response.text();
    let parsed: unknown = text;
    try {
      parsed = text.length > 0 ? JSON.parse(text) : undefined;
    } catch {
      /* non-JSON body: return the text as-is */
    }
    return { status: response.status, body: parsed };
  } catch (error) {
    return {
      error: {
        code: "offline",
        msg: error instanceof Error ? error.message : String(error),
      },
    };
  }
}
