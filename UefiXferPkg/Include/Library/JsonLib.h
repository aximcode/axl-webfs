/** @file
  JsonLib — Minimal JSON parser for directory listings.

  Parses flat JSON arrays of objects as returned by xfer-server.py.
  No dynamic allocation — uses a fixed-size token array in JSON_CTX.
  Designed for parsing: [{"name":"foo","size":123,"dir":false,"modified":"..."},...]

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef JSON_LIB_H_
#define JSON_LIB_H_

#include <Uefi.h>

/// Maximum tokens in a single JSON parse. Supports ~30 directory entries
/// (each entry uses ~9 tokens: object + 4 key-value pairs).
#define JSON_MAX_TOKENS  256

/// Token types identified by the parser.
typedef enum {
    JSON_TOKEN_NONE,
    JSON_TOKEN_OBJECT_START,   ///< {
    JSON_TOKEN_OBJECT_END,     ///< }
    JSON_TOKEN_ARRAY_START,    ///< [
    JSON_TOKEN_ARRAY_END,      ///< ]
    JSON_TOKEN_STRING,         ///< "..."
    JSON_TOKEN_NUMBER,         ///< 123, -45
    JSON_TOKEN_TRUE,           ///< true
    JSON_TOKEN_FALSE,          ///< false
    JSON_TOKEN_NULL,           ///< null
} JSON_TOKEN_TYPE;

/// A single token pointing into the original JSON string.
typedef struct {
    JSON_TOKEN_TYPE  Type;
    UINTN            Start;    ///< Offset into JSON string (after opening quote for strings).
    UINTN            End;      ///< Offset of end (before closing quote for strings).
} JSON_TOKEN;

/// Parser context. Stack-allocatable.
typedef struct {
    CONST CHAR8   *Json;
    UINTN         JsonLen;
    JSON_TOKEN    Tokens[JSON_MAX_TOKENS];
    UINTN         TokenCount;
} JSON_CTX;

/// Iterator for walking array elements.
typedef struct {
    CONST JSON_CTX  *Ctx;
    UINTN           ArrayStart;   ///< Token index of the '[' token.
    UINTN           Current;      ///< Token index of current element (or end).
    UINTN           Count;        ///< Total number of elements.
    UINTN           Index;        ///< Current element index (0-based).
} JSON_ARRAY_ITER;

/**
  Parse a JSON string into tokens.

  @param[in]  Json     The JSON string to parse.
  @param[in]  JsonLen  Length of the JSON string.
  @param[out] Ctx      Parser context with populated token array.

  @retval EFI_SUCCESS            Parsed successfully.
  @retval EFI_INVALID_PARAMETER  Malformed JSON.
  @retval EFI_BUFFER_TOO_SMALL   Too many tokens (increase JSON_MAX_TOKENS).
**/
EFI_STATUS
EFIAPI
JsonParse (
    IN  CONST CHAR8  *Json,
    IN  UINTN        JsonLen,
    OUT JSON_CTX     *Ctx
    );

/**
  Extract a string value by key from an object.

  Searches for "Key":"Value" in the object starting at token ObjStart.

  @param[in]  Ctx       Parser context.
  @param[in]  ObjStart  Token index of the '{' token for the object.
  @param[in]  Key       Key name to find.
  @param[out] Buf       Buffer to receive the string value (null-terminated).
  @param[in]  BufSize   Size of Buf.

  @retval EFI_SUCCESS         Value extracted.
  @retval EFI_NOT_FOUND       Key not found in object.
  @retval EFI_BUFFER_TOO_SMALL  Value truncated.
**/
EFI_STATUS
EFIAPI
JsonGetString (
    IN  CONST JSON_CTX  *Ctx,
    IN  UINTN           ObjStart,
    IN  CONST CHAR8     *Key,
    OUT CHAR8           *Buf,
    IN  UINTN           BufSize
    );

/**
  Extract a numeric value by key from an object.

  @param[in]  Ctx       Parser context.
  @param[in]  ObjStart  Token index of the '{' token for the object.
  @param[in]  Key       Key name to find.
  @param[out] Value     The numeric value.

  @retval EFI_SUCCESS     Value extracted.
  @retval EFI_NOT_FOUND   Key not found in object.
**/
EFI_STATUS
EFIAPI
JsonGetNumber (
    IN  CONST JSON_CTX  *Ctx,
    IN  UINTN           ObjStart,
    IN  CONST CHAR8     *Key,
    OUT UINTN           *Value
    );

/**
  Extract a boolean value by key from an object.

  @param[in]  Ctx       Parser context.
  @param[in]  ObjStart  Token index of the '{' token for the object.
  @param[in]  Key       Key name to find.
  @param[out] Value     TRUE or FALSE.

  @retval EFI_SUCCESS     Value extracted.
  @retval EFI_NOT_FOUND   Key not found in object.
**/
EFI_STATUS
EFIAPI
JsonGetBool (
    IN  CONST JSON_CTX  *Ctx,
    IN  UINTN           ObjStart,
    IN  CONST CHAR8     *Key,
    OUT BOOLEAN         *Value
    );

/**
  Begin iterating elements of a JSON array.

  If the root of the parsed JSON is an array, pass ObjStart = 0.

  @param[in]  Ctx       Parser context.
  @param[in]  ArrayIdx  Token index of the '[' token.
  @param[out] Iter      Initialized array iterator.

  @retval EFI_SUCCESS            Iterator ready.
  @retval EFI_INVALID_PARAMETER  Token at ArrayIdx is not '['.
**/
EFI_STATUS
EFIAPI
JsonArrayFirst (
    IN  CONST JSON_CTX    *Ctx,
    IN  UINTN             ArrayIdx,
    OUT JSON_ARRAY_ITER   *Iter
    );

/**
  Advance to the next element in a JSON array.

  @param[in,out] Iter      Array iterator.
  @param[out]    ElemIdx   Token index of the next element (object, array, or value).

  @retval EFI_SUCCESS      Element returned.
  @retval EFI_NOT_FOUND    No more elements (iteration complete).
**/
EFI_STATUS
EFIAPI
JsonArrayNext (
    IN OUT JSON_ARRAY_ITER  *Iter,
    OUT    UINTN            *ElemIdx
    );

/**
  Get the number of elements in a JSON array.

  @param[in] Ctx       Parser context.
  @param[in] ArrayIdx  Token index of the '[' token.

  @return Number of top-level elements in the array, or 0 on error.
**/
UINTN
EFIAPI
JsonArrayCount (
    IN CONST JSON_CTX  *Ctx,
    IN UINTN           ArrayIdx
    );

#endif // JSON_LIB_H_
