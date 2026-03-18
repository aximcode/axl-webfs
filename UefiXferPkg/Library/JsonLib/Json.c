/** @file
  JsonLib — Minimal JSON parser for directory listings.

  Custom tokenizer for the flat JSON arrays returned by xfer-server.py.
  No dynamic allocation — fixed-size token array in JSON_CTX.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/JsonLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>

// ============================================================================
// Tokenizer
// ============================================================================

/// Skip whitespace, return new position.
static UINTN SkipWs(CONST CHAR8 *Json, UINTN Len, UINTN Pos) {
    while (Pos < Len) {
        CHAR8 Ch = Json[Pos];
        if (Ch != ' ' && Ch != '\t' && Ch != '\r' && Ch != '\n') break;
        Pos++;
    }
    return Pos;
}

/// Add a token to the context. Returns FALSE if full.
static BOOLEAN AddToken(
    IN OUT JSON_CTX      *Ctx,
    IN     JSON_TOKEN_TYPE Type,
    IN     UINTN          Start,
    IN     UINTN          End
) {
    if (Ctx->TokenCount >= JSON_MAX_TOKENS) return FALSE;
    Ctx->Tokens[Ctx->TokenCount].Type = Type;
    Ctx->Tokens[Ctx->TokenCount].Start = Start;
    Ctx->Tokens[Ctx->TokenCount].End = End;
    Ctx->TokenCount++;
    return TRUE;
}

/// Parse a string token (Pos points to opening '"'). Returns position after closing '"'.
static UINTN ParseString(
    IN     CONST CHAR8  *Json,
    IN     UINTN        Len,
    IN     UINTN        Pos,
    IN OUT JSON_CTX     *Ctx
) {
    Pos++;  // Skip opening '"'
    UINTN Start = Pos;

    while (Pos < Len) {
        if (Json[Pos] == '\\') {
            Pos += 2;  // Skip escaped char
            continue;
        }
        if (Json[Pos] == '"') {
            AddToken(Ctx, JSON_TOKEN_STRING, Start, Pos);
            return Pos + 1;
        }
        Pos++;
    }
    return Len;  // Unterminated string
}

/// Parse a number token. Returns position after the number.
static UINTN ParseNumber(
    IN     CONST CHAR8  *Json,
    IN     UINTN        Len,
    IN     UINTN        Pos,
    IN OUT JSON_CTX     *Ctx
) {
    UINTN Start = Pos;
    if (Pos < Len && Json[Pos] == '-') Pos++;
    while (Pos < Len && Json[Pos] >= '0' && Json[Pos] <= '9') Pos++;
    // Handle decimal and exponent
    if (Pos < Len && Json[Pos] == '.') {
        Pos++;
        while (Pos < Len && Json[Pos] >= '0' && Json[Pos] <= '9') Pos++;
    }
    if (Pos < Len && (Json[Pos] == 'e' || Json[Pos] == 'E')) {
        Pos++;
        if (Pos < Len && (Json[Pos] == '+' || Json[Pos] == '-')) Pos++;
        while (Pos < Len && Json[Pos] >= '0' && Json[Pos] <= '9') Pos++;
    }
    AddToken(Ctx, JSON_TOKEN_NUMBER, Start, Pos);
    return Pos;
}

/// Parse a literal (true/false/null). Returns position after the literal.
static UINTN ParseLiteral(
    IN     CONST CHAR8   *Json,
    IN     UINTN         Len,
    IN     UINTN         Pos,
    IN OUT JSON_CTX      *Ctx
) {
    if (Pos + 4 <= Len && Json[Pos] == 't' && Json[Pos+1] == 'r' &&
        Json[Pos+2] == 'u' && Json[Pos+3] == 'e') {
        AddToken(Ctx, JSON_TOKEN_TRUE, Pos, Pos + 4);
        return Pos + 4;
    }
    if (Pos + 5 <= Len && Json[Pos] == 'f' && Json[Pos+1] == 'a' &&
        Json[Pos+2] == 'l' && Json[Pos+3] == 's' && Json[Pos+4] == 'e') {
        AddToken(Ctx, JSON_TOKEN_FALSE, Pos, Pos + 5);
        return Pos + 5;
    }
    if (Pos + 4 <= Len && Json[Pos] == 'n' && Json[Pos+1] == 'u' &&
        Json[Pos+2] == 'l' && Json[Pos+3] == 'l') {
        AddToken(Ctx, JSON_TOKEN_NULL, Pos, Pos + 4);
        return Pos + 4;
    }
    return Len;  // Invalid
}

EFI_STATUS
EFIAPI
JsonParse (
    IN  CONST CHAR8  *Json,
    IN  UINTN        JsonLen,
    OUT JSON_CTX     *Ctx
    )
{
    if (Json == NULL || Ctx == NULL) return EFI_INVALID_PARAMETER;

    SetMem(Ctx, sizeof(*Ctx), 0);
    Ctx->Json = Json;
    Ctx->JsonLen = JsonLen;

    UINTN Pos = 0;
    while (Pos < JsonLen) {
        Pos = SkipWs(Json, JsonLen, Pos);
        if (Pos >= JsonLen) break;

        CHAR8 Ch = Json[Pos];

        if (Ch == '{') {
            if (!AddToken(Ctx, JSON_TOKEN_OBJECT_START, Pos, Pos + 1))
                return EFI_BUFFER_TOO_SMALL;
            Pos++;
        } else if (Ch == '}') {
            if (!AddToken(Ctx, JSON_TOKEN_OBJECT_END, Pos, Pos + 1))
                return EFI_BUFFER_TOO_SMALL;
            Pos++;
        } else if (Ch == '[') {
            if (!AddToken(Ctx, JSON_TOKEN_ARRAY_START, Pos, Pos + 1))
                return EFI_BUFFER_TOO_SMALL;
            Pos++;
        } else if (Ch == ']') {
            if (!AddToken(Ctx, JSON_TOKEN_ARRAY_END, Pos, Pos + 1))
                return EFI_BUFFER_TOO_SMALL;
            Pos++;
        } else if (Ch == '"') {
            UINTN OldCount = Ctx->TokenCount;
            Pos = ParseString(Json, JsonLen, Pos, Ctx);
            if (Ctx->TokenCount == OldCount) return EFI_INVALID_PARAMETER;
        } else if (Ch == '-' || (Ch >= '0' && Ch <= '9')) {
            Pos = ParseNumber(Json, JsonLen, Pos, Ctx);
        } else if (Ch == 't' || Ch == 'f' || Ch == 'n') {
            Pos = ParseLiteral(Json, JsonLen, Pos, Ctx);
        } else if (Ch == ',' || Ch == ':') {
            Pos++;  // Skip delimiters
        } else {
            return EFI_INVALID_PARAMETER;  // Unexpected character
        }
    }

    return EFI_SUCCESS;
}

// ============================================================================
// Key lookup helpers
// ============================================================================

/// Find the token index of a key's value within an object starting at ObjStart.
/// ObjStart must point to a JSON_TOKEN_OBJECT_START token.
/// Returns (UINTN)-1 if not found.
static UINTN FindKeyValue(
    IN CONST JSON_CTX  *Ctx,
    IN UINTN           ObjStart,
    IN CONST CHAR8     *Key
) {
    if (ObjStart >= Ctx->TokenCount) return (UINTN)-1;
    if (Ctx->Tokens[ObjStart].Type != JSON_TOKEN_OBJECT_START) return (UINTN)-1;

    UINTN KeyLen = AsciiStrLen(Key);
    UINTN Depth = 0;

    for (UINTN i = ObjStart + 1; i < Ctx->TokenCount; i++) {
        JSON_TOKEN *Tok = &((JSON_CTX *)Ctx)->Tokens[i];

        if (Tok->Type == JSON_TOKEN_OBJECT_START || Tok->Type == JSON_TOKEN_ARRAY_START) {
            Depth++;
            continue;
        }
        if (Tok->Type == JSON_TOKEN_OBJECT_END || Tok->Type == JSON_TOKEN_ARRAY_END) {
            if (Depth == 0) break;  // End of our object
            Depth--;
            continue;
        }

        if (Depth > 0) continue;  // Inside nested structure

        // At depth 0: this should be a key string, followed by a value
        if (Tok->Type == JSON_TOKEN_STRING) {
            UINTN TokLen = Tok->End - Tok->Start;
            if (TokLen == KeyLen &&
                CompareMem(Ctx->Json + Tok->Start, Key, KeyLen) == 0) {
                // Next token is the value
                if (i + 1 < Ctx->TokenCount) return i + 1;
            }
            // Skip the value token (might be complex)
            i++;  // Will be incremented by loop
        }
    }

    return (UINTN)-1;
}

EFI_STATUS
EFIAPI
JsonGetString (
    IN  CONST JSON_CTX  *Ctx,
    IN  UINTN           ObjStart,
    IN  CONST CHAR8     *Key,
    OUT CHAR8           *Buf,
    IN  UINTN           BufSize
    )
{
    UINTN Idx = FindKeyValue(Ctx, ObjStart, Key);
    if (Idx == (UINTN)-1) return EFI_NOT_FOUND;

    JSON_TOKEN *Tok = &((JSON_CTX *)Ctx)->Tokens[Idx];
    if (Tok->Type != JSON_TOKEN_STRING) return EFI_NOT_FOUND;

    UINTN Len = Tok->End - Tok->Start;
    if (Len >= BufSize) {
        CopyMem(Buf, Ctx->Json + Tok->Start, BufSize - 1);
        Buf[BufSize - 1] = '\0';
        return EFI_BUFFER_TOO_SMALL;
    }

    CopyMem(Buf, Ctx->Json + Tok->Start, Len);
    Buf[Len] = '\0';
    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
JsonGetNumber (
    IN  CONST JSON_CTX  *Ctx,
    IN  UINTN           ObjStart,
    IN  CONST CHAR8     *Key,
    OUT UINTN           *Value
    )
{
    UINTN Idx = FindKeyValue(Ctx, ObjStart, Key);
    if (Idx == (UINTN)-1) return EFI_NOT_FOUND;

    JSON_TOKEN *Tok = &((JSON_CTX *)Ctx)->Tokens[Idx];
    if (Tok->Type != JSON_TOKEN_NUMBER) return EFI_NOT_FOUND;

    // Parse decimal number
    UINTN Val = 0;
    for (UINTN i = Tok->Start; i < Tok->End; i++) {
        CHAR8 Ch = Ctx->Json[i];
        if (Ch < '0' || Ch > '9') break;
        Val = Val * 10 + (Ch - '0');
    }
    *Value = Val;
    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
JsonGetBool (
    IN  CONST JSON_CTX  *Ctx,
    IN  UINTN           ObjStart,
    IN  CONST CHAR8     *Key,
    OUT BOOLEAN         *Value
    )
{
    UINTN Idx = FindKeyValue(Ctx, ObjStart, Key);
    if (Idx == (UINTN)-1) return EFI_NOT_FOUND;

    JSON_TOKEN *Tok = &((JSON_CTX *)Ctx)->Tokens[Idx];
    if (Tok->Type == JSON_TOKEN_TRUE) {
        *Value = TRUE;
        return EFI_SUCCESS;
    }
    if (Tok->Type == JSON_TOKEN_FALSE) {
        *Value = FALSE;
        return EFI_SUCCESS;
    }
    return EFI_NOT_FOUND;
}

// ============================================================================
// Array iteration
// ============================================================================

/// Skip a token and all its children (for objects/arrays). Returns next token index.
static UINTN SkipValue(CONST JSON_CTX *Ctx, UINTN Idx) {
    if (Idx >= Ctx->TokenCount) return Idx;

    JSON_TOKEN_TYPE Type = Ctx->Tokens[Idx].Type;

    if (Type == JSON_TOKEN_OBJECT_START) {
        UINTN Depth = 1;
        for (UINTN i = Idx + 1; i < Ctx->TokenCount; i++) {
            if (Ctx->Tokens[i].Type == JSON_TOKEN_OBJECT_START) Depth++;
            if (Ctx->Tokens[i].Type == JSON_TOKEN_OBJECT_END) {
                Depth--;
                if (Depth == 0) return i + 1;
            }
        }
        return Ctx->TokenCount;
    }

    if (Type == JSON_TOKEN_ARRAY_START) {
        UINTN Depth = 1;
        for (UINTN i = Idx + 1; i < Ctx->TokenCount; i++) {
            if (Ctx->Tokens[i].Type == JSON_TOKEN_ARRAY_START) Depth++;
            if (Ctx->Tokens[i].Type == JSON_TOKEN_ARRAY_END) {
                Depth--;
                if (Depth == 0) return i + 1;
            }
        }
        return Ctx->TokenCount;
    }

    // Primitive value: just skip one token
    return Idx + 1;
}

EFI_STATUS
EFIAPI
JsonArrayFirst (
    IN  CONST JSON_CTX    *Ctx,
    IN  UINTN             ArrayIdx,
    OUT JSON_ARRAY_ITER   *Iter
    )
{
    if (Ctx == NULL || Iter == NULL) return EFI_INVALID_PARAMETER;
    if (ArrayIdx >= Ctx->TokenCount) return EFI_INVALID_PARAMETER;
    if (Ctx->Tokens[ArrayIdx].Type != JSON_TOKEN_ARRAY_START) {
        return EFI_INVALID_PARAMETER;
    }

    SetMem(Iter, sizeof(*Iter), 0);
    Iter->Ctx = Ctx;
    Iter->ArrayStart = ArrayIdx;
    Iter->Current = ArrayIdx + 1;  // First element
    Iter->Index = 0;

    // Count elements
    Iter->Count = JsonArrayCount(Ctx, ArrayIdx);

    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
JsonArrayNext (
    IN OUT JSON_ARRAY_ITER  *Iter,
    OUT    UINTN            *ElemIdx
    )
{
    if (Iter == NULL || ElemIdx == NULL) return EFI_INVALID_PARAMETER;

    CONST JSON_CTX *Ctx = Iter->Ctx;

    if (Iter->Current >= Ctx->TokenCount) return EFI_NOT_FOUND;
    if (Ctx->Tokens[Iter->Current].Type == JSON_TOKEN_ARRAY_END) {
        return EFI_NOT_FOUND;
    }

    *ElemIdx = Iter->Current;
    Iter->Current = SkipValue(Ctx, Iter->Current);
    Iter->Index++;

    return EFI_SUCCESS;
}

UINTN
EFIAPI
JsonArrayCount (
    IN CONST JSON_CTX  *Ctx,
    IN UINTN           ArrayIdx
    )
{
    if (Ctx == NULL || ArrayIdx >= Ctx->TokenCount) return 0;
    if (Ctx->Tokens[ArrayIdx].Type != JSON_TOKEN_ARRAY_START) return 0;

    UINTN Count = 0;
    UINTN Pos = ArrayIdx + 1;

    while (Pos < Ctx->TokenCount) {
        if (Ctx->Tokens[Pos].Type == JSON_TOKEN_ARRAY_END) break;
        Count++;
        Pos = SkipValue(Ctx, Pos);
    }

    return Count;
}
