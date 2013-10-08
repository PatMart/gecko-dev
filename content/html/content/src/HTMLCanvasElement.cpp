/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/HTMLCanvasElement.h"

#include "Layers.h"
#include "imgIEncoder.h"
#include "jsapi.h"
#include "jsfriendapi.h"
#include "gfxImageSurface.h"
#include "mozilla/Base64.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/dom/CanvasRenderingContext2D.h"
#include "mozilla/dom/HTMLCanvasElementBinding.h"
#include "mozilla/dom/UnionTypes.h"
#include "mozilla/gfx/Rect.h"
#include "mozilla/Preferences.h"
#include "mozilla/Telemetry.h"
#include "nsAsyncDOMEvent.h"
#include "nsAttrValueInlines.h"
#include "nsContentUtils.h"
#include "nsDisplayList.h"
#include "nsDOMFile.h"
#include "nsFrameManager.h"
#include "nsIScriptSecurityManager.h"
#include "nsITimer.h"
#include "nsIWritablePropertyBag2.h"
#include "nsIXPConnect.h"
#include "nsJSUtils.h"
#include "nsMathUtils.h"
#include "nsNetUtil.h"
#include "nsStreamUtils.h"

#ifdef MOZ_WEBGL
#include "../canvas/src/WebGL2Context.h"
#endif

using namespace mozilla::layers;

NS_IMPL_NS_NEW_HTML_ELEMENT(Canvas)

namespace {

typedef mozilla::dom::HTMLImageElementOrHTMLCanvasElementOrHTMLVideoElement
HTMLImageOrCanvasOrVideoElement;

class ToBlobRunnable : public nsRunnable
{
public:
  ToBlobRunnable(mozilla::dom::FileCallback& aCallback,
                 nsIDOMBlob* aBlob)
    : mCallback(&aCallback),
      mBlob(aBlob)
  {
    NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  }

  NS_IMETHOD Run()
  {
    NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
    mozilla::ErrorResult rv;
    mCallback->Call(mBlob, rv);
    return rv.ErrorCode();
  }
private:
  nsRefPtr<mozilla::dom::FileCallback> mCallback;
  nsCOMPtr<nsIDOMBlob> mBlob;
};

} // anonymous namespace

namespace mozilla {
namespace dom {

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE_3(HTMLCanvasPrintState, mCanvas,
                                        mContext, mCallback)

NS_IMPL_CYCLE_COLLECTION_ROOT_NATIVE(HTMLCanvasPrintState, AddRef)
NS_IMPL_CYCLE_COLLECTION_UNROOT_NATIVE(HTMLCanvasPrintState, Release)

HTMLCanvasPrintState::HTMLCanvasPrintState(HTMLCanvasElement* aCanvas,
                                           nsICanvasRenderingContextInternal* aContext,
                                           nsITimerCallback* aCallback)
  : mIsDone(false), mPendingNotify(false), mCanvas(aCanvas),
    mContext(aContext), mCallback(aCallback)
{
  SetIsDOMBinding();
}

HTMLCanvasPrintState::~HTMLCanvasPrintState()
{
}

/* virtual */ JSObject*
HTMLCanvasPrintState::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aScope)
{
  return MozCanvasPrintStateBinding::Wrap(aCx, aScope, this);
}

nsISupports*
HTMLCanvasPrintState::Context() const
{
  return mContext;
}

void
HTMLCanvasPrintState::Done()
{
  if (!mPendingNotify && !mIsDone) {
    // The canvas needs to be invalidated for printing reftests on linux to
    // work.
    if (mCanvas) {
      mCanvas->InvalidateCanvas();
    }
    nsRefPtr<nsRunnableMethod<HTMLCanvasPrintState> > doneEvent =
      NS_NewRunnableMethod(this, &HTMLCanvasPrintState::NotifyDone);
    if (NS_SUCCEEDED(NS_DispatchToCurrentThread(doneEvent))) {
      mPendingNotify = true;
    }
  }
}

void
HTMLCanvasPrintState::NotifyDone()
{
  mIsDone = true;
  mPendingNotify = false;
  if (mCallback) {
    mCallback->Notify(nullptr);
  }
}

// ---------------------------------------------------------------------------

HTMLCanvasElement::HTMLCanvasElement(already_AddRefed<nsINodeInfo> aNodeInfo)
  : nsGenericHTMLElement(aNodeInfo),
    mWriteOnly(false)
{
}

HTMLCanvasElement::~HTMLCanvasElement()
{
  ResetPrintCallback();
}

NS_IMPL_CYCLE_COLLECTION_INHERITED_4(HTMLCanvasElement, nsGenericHTMLElement,
                                     mCurrentContext, mPrintCallback,
                                     mPrintState, mOriginalCanvas)

NS_IMPL_ADDREF_INHERITED(HTMLCanvasElement, Element)
NS_IMPL_RELEASE_INHERITED(HTMLCanvasElement, Element)

NS_INTERFACE_TABLE_HEAD_CYCLE_COLLECTION_INHERITED(HTMLCanvasElement)
  NS_INTERFACE_TABLE_INHERITED2(HTMLCanvasElement,
                                nsIDOMHTMLCanvasElement,
                                nsICanvasElementExternal)
NS_INTERFACE_TABLE_TAIL_INHERITING(nsGenericHTMLElement)

NS_IMPL_ELEMENT_CLONE(HTMLCanvasElement)

/* virtual */ JSObject*
HTMLCanvasElement::WrapNode(JSContext* aCx, JS::Handle<JSObject*> aScope)
{
  return HTMLCanvasElementBinding::Wrap(aCx, aScope, this);
}

nsIntSize
HTMLCanvasElement::GetWidthHeight()
{
  nsIntSize size(DEFAULT_CANVAS_WIDTH, DEFAULT_CANVAS_HEIGHT);
  const nsAttrValue* value;

  if ((value = GetParsedAttr(nsGkAtoms::width)) &&
      value->Type() == nsAttrValue::eInteger)
  {
      size.width = value->GetIntegerValue();
  }

  if ((value = GetParsedAttr(nsGkAtoms::height)) &&
      value->Type() == nsAttrValue::eInteger)
  {
      size.height = value->GetIntegerValue();
  }

  return size;
}

NS_IMPL_UINT_ATTR_DEFAULT_VALUE(HTMLCanvasElement, Width, width, DEFAULT_CANVAS_WIDTH)
NS_IMPL_UINT_ATTR_DEFAULT_VALUE(HTMLCanvasElement, Height, height, DEFAULT_CANVAS_HEIGHT)
NS_IMPL_BOOL_ATTR(HTMLCanvasElement, MozOpaque, moz_opaque)

nsresult
HTMLCanvasElement::SetAttr(int32_t aNameSpaceID, nsIAtom* aName,
                           nsIAtom* aPrefix, const nsAString& aValue,
                           bool aNotify)
{
  nsresult rv = nsGenericHTMLElement::SetAttr(aNameSpaceID, aName, aPrefix, aValue,
                                              aNotify);
  if (NS_SUCCEEDED(rv) && mCurrentContext &&
      aNameSpaceID == kNameSpaceID_None &&
      (aName == nsGkAtoms::width || aName == nsGkAtoms::height || aName == nsGkAtoms::moz_opaque))
  {
    rv = UpdateContext(nullptr, JS::NullHandleValue);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  return rv;
}

nsresult
HTMLCanvasElement::UnsetAttr(int32_t aNameSpaceID, nsIAtom* aName,
                             bool aNotify)
{
  nsresult rv = nsGenericHTMLElement::UnsetAttr(aNameSpaceID, aName, aNotify);
  if (NS_SUCCEEDED(rv) && mCurrentContext &&
      aNameSpaceID == kNameSpaceID_None &&
      (aName == nsGkAtoms::width || aName == nsGkAtoms::height || aName == nsGkAtoms::moz_opaque))
  {
    rv = UpdateContext(nullptr, JS::NullHandleValue);
    NS_ENSURE_SUCCESS(rv, rv);
  }
  return rv;
}

void
HTMLCanvasElement::HandlePrintCallback(nsPresContext::nsPresContextType aType)
{
  // Only call the print callback here if 1) we're in a print testing mode or
  // print preview mode, 2) the canvas has a print callback and 3) the callback
  // hasn't already been called. For real printing the callback is handled in
  // nsSimplePageSequenceFrame::PrePrintNextPage.
  if ((aType == nsPresContext::eContext_PageLayout ||
       aType == nsPresContext::eContext_PrintPreview) &&
      !mPrintState && GetMozPrintCallback()) {
    DispatchPrintCallback(nullptr);
  }
}

nsresult
HTMLCanvasElement::DispatchPrintCallback(nsITimerCallback* aCallback)
{
  // For print reftests the context may not be initialized yet, so get a context
  // so mCurrentContext is set.
  if (!mCurrentContext) {
    nsresult rv;
    nsCOMPtr<nsISupports> context;
    rv = GetContext(NS_LITERAL_STRING("2d"), getter_AddRefs(context));
    NS_ENSURE_SUCCESS(rv, rv);
  }
  mPrintState = new HTMLCanvasPrintState(this, mCurrentContext, aCallback);

  nsRefPtr<nsRunnableMethod<HTMLCanvasElement> > renderEvent =
        NS_NewRunnableMethod(this, &HTMLCanvasElement::CallPrintCallback);
  return NS_DispatchToCurrentThread(renderEvent);
}

void
HTMLCanvasElement::CallPrintCallback()
{
  ErrorResult rv;
  GetMozPrintCallback()->Call(*mPrintState, rv);
}

void
HTMLCanvasElement::ResetPrintCallback()
{
  if (mPrintState) {
    mPrintState = nullptr;
  }
}

bool
HTMLCanvasElement::IsPrintCallbackDone()
{
  if (mPrintState == nullptr) {
    return true;
  }

  return mPrintState->mIsDone;
}

HTMLCanvasElement*
HTMLCanvasElement::GetOriginalCanvas()
{
  return mOriginalCanvas ? mOriginalCanvas.get() : this;
}

nsresult
HTMLCanvasElement::CopyInnerTo(Element* aDest)
{
  nsresult rv = nsGenericHTMLElement::CopyInnerTo(aDest);
  NS_ENSURE_SUCCESS(rv, rv);
  if (aDest->OwnerDoc()->IsStaticDocument()) {
    HTMLCanvasElement* dest = static_cast<HTMLCanvasElement*>(aDest);
    dest->mOriginalCanvas = this;

    nsCOMPtr<nsISupports> cxt;
    dest->GetContext(NS_LITERAL_STRING("2d"), getter_AddRefs(cxt));
    nsRefPtr<CanvasRenderingContext2D> context2d =
      static_cast<CanvasRenderingContext2D*>(cxt.get());
    if (context2d && !mPrintCallback) {
      HTMLImageOrCanvasOrVideoElement element;
      element.SetAsHTMLCanvasElement() = this;
      ErrorResult err;
      context2d->DrawImage(element,
                           0.0, 0.0, err);
      rv = err.ErrorCode();
    }
  }
  return rv;
}

nsChangeHint
HTMLCanvasElement::GetAttributeChangeHint(const nsIAtom* aAttribute,
                                          int32_t aModType) const
{
  nsChangeHint retval =
    nsGenericHTMLElement::GetAttributeChangeHint(aAttribute, aModType);
  if (aAttribute == nsGkAtoms::width ||
      aAttribute == nsGkAtoms::height)
  {
    NS_UpdateHint(retval, NS_STYLE_HINT_REFLOW);
  } else if (aAttribute == nsGkAtoms::moz_opaque)
  {
    NS_UpdateHint(retval, NS_STYLE_HINT_VISUAL);
  }
  return retval;
}

bool
HTMLCanvasElement::ParseAttribute(int32_t aNamespaceID,
                                  nsIAtom* aAttribute,
                                  const nsAString& aValue,
                                  nsAttrValue& aResult)
{
  if (aNamespaceID == kNameSpaceID_None &&
      (aAttribute == nsGkAtoms::width || aAttribute == nsGkAtoms::height)) {
    return aResult.ParseNonNegativeIntValue(aValue);
  }

  return nsGenericHTMLElement::ParseAttribute(aNamespaceID, aAttribute, aValue,
                                              aResult);
}


// HTMLCanvasElement::toDataURL

NS_IMETHODIMP
HTMLCanvasElement::ToDataURL(const nsAString& aType, const JS::Value& aParams,
                             JSContext* aCx, nsAString& aDataURL)
{
  // do a trust check if this is a write-only canvas
  if (mWriteOnly && !nsContentUtils::IsCallerChrome()) {
    return NS_ERROR_DOM_SECURITY_ERR;
  }

  return ToDataURLImpl(aCx, aType, aParams, aDataURL);
}

// HTMLCanvasElement::mozFetchAsStream

NS_IMETHODIMP
HTMLCanvasElement::MozFetchAsStream(nsIInputStreamCallback *aCallback,
                                    const nsAString& aType)
{
  if (!nsContentUtils::IsCallerChrome())
    return NS_ERROR_FAILURE;

  nsresult rv;
  bool fellBackToPNG = false;
  nsCOMPtr<nsIInputStream> inputData;

  rv = ExtractData(aType, EmptyString(), getter_AddRefs(inputData), fellBackToPNG);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIAsyncInputStream> asyncData = do_QueryInterface(inputData, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIThread> mainThread;
  rv = NS_GetMainThread(getter_AddRefs(mainThread));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIInputStreamCallback> asyncCallback =
    NS_NewInputStreamReadyEvent(aCallback, mainThread);

  return asyncCallback->OnInputStreamReady(asyncData);
}

void
HTMLCanvasElement::SetMozPrintCallback(PrintCallback* aCallback)
{
  mPrintCallback = aCallback;
}

PrintCallback*
HTMLCanvasElement::GetMozPrintCallback() const
{
  if (mOriginalCanvas) {
    return mOriginalCanvas->GetMozPrintCallback();
  }
  return mPrintCallback;
}

nsresult
HTMLCanvasElement::ExtractData(const nsAString& aType,
                               const nsAString& aOptions,
                               nsIInputStream** aStream,
                               bool& aFellBackToPNG)
{
  // note that if we don't have a current context, the spec says we're
  // supposed to just return transparent black pixels of the canvas
  // dimensions.
  nsRefPtr<gfxImageSurface> emptyCanvas;
  nsIntSize size = GetWidthHeight();
  if (!mCurrentContext) {
    emptyCanvas = new gfxImageSurface(gfxIntSize(size.width, size.height), gfxImageFormatARGB32);
    if (emptyCanvas->CairoStatus()) {
      return NS_ERROR_INVALID_ARG;
    }
  }

  nsresult rv;

  // get image bytes
  nsCOMPtr<nsIInputStream> imgStream;
  NS_ConvertUTF16toUTF8 encoderType(aType);

 try_again:
  if (mCurrentContext) {
    rv = mCurrentContext->GetInputStream(encoderType.get(),
                                         nsPromiseFlatString(aOptions).get(),
                                         getter_AddRefs(imgStream));
  } else {
    // no context, so we have to encode the empty image we created above
    nsCString enccid("@mozilla.org/image/encoder;2?type=");
    enccid += encoderType;

    nsCOMPtr<imgIEncoder> encoder = do_CreateInstance(enccid.get(), &rv);
    if (NS_SUCCEEDED(rv) && encoder) {
      rv = encoder->InitFromData(emptyCanvas->Data(),
                                 size.width * size.height * 4,
                                 size.width,
                                 size.height,
                                 size.width * 4,
                                 imgIEncoder::INPUT_FORMAT_HOSTARGB,
                                 aOptions);
      if (NS_SUCCEEDED(rv)) {
        imgStream = do_QueryInterface(encoder);
      }
    } else {
      rv = NS_ERROR_FAILURE;
    }
  }

  if (NS_FAILED(rv) && !aFellBackToPNG) {
    // Try image/png instead.
    // XXX ERRMSG we need to report an error to developers here! (bug 329026)
    aFellBackToPNG = true;
    encoderType.AssignLiteral("image/png");
    goto try_again;
  }

  NS_ENSURE_SUCCESS(rv, rv);

  imgStream.forget(aStream);
  return NS_OK;
}

nsresult
HTMLCanvasElement::ParseParams(JSContext* aCx,
                               const nsAString& aType,
                               const JS::Value& aEncoderOptions,
                               nsAString& aParams,
                               bool* usingCustomParseOptions)
{
  // Quality parameter is only valid for the image/jpeg MIME type
  if (aType.EqualsLiteral("image/jpeg")) {
    if (aEncoderOptions.isNumber()) {
      double quality = aEncoderOptions.toNumber();
      // Quality must be between 0.0 and 1.0, inclusive
      if (quality >= 0.0 && quality <= 1.0) {
        aParams.AppendLiteral("quality=");
        aParams.AppendInt(NS_lround(quality * 100.0));
      }
    }
  }

  // If we haven't parsed the aParams check for proprietary options.
  // The proprietary option -moz-parse-options will take a image lib encoder
  // parse options string as is and pass it to the encoder.
  *usingCustomParseOptions = false;
  if (aParams.Length() == 0 && aEncoderOptions.isString()) {
    NS_NAMED_LITERAL_STRING(mozParseOptions, "-moz-parse-options:");
    nsDependentJSString paramString;
    if (!paramString.init(aCx, aEncoderOptions.toString())) {
      return NS_ERROR_FAILURE;
    }
    if (StringBeginsWith(paramString, mozParseOptions)) {
      nsDependentSubstring parseOptions = Substring(paramString,
                                                    mozParseOptions.Length(),
                                                    paramString.Length() -
                                                    mozParseOptions.Length());
      aParams.Append(parseOptions);
      *usingCustomParseOptions = true;
    }
  }

  return NS_OK;
}

nsresult
HTMLCanvasElement::ToDataURLImpl(JSContext* aCx,
                                 const nsAString& aMimeType,
                                 const JS::Value& aEncoderOptions,
                                 nsAString& aDataURL)
{
  bool fallbackToPNG = false;

  nsIntSize size = GetWidthHeight();
  if (size.height == 0 || size.width == 0) {
    aDataURL = NS_LITERAL_STRING("data:,");
    return NS_OK;
  }

  nsAutoString type;
  nsresult rv = nsContentUtils::ASCIIToLower(aMimeType, type);
  if (NS_FAILED(rv)) {
    return rv;
  }

  nsAutoString params;
  bool usingCustomParseOptions;
  rv = ParseParams(aCx, type, aEncoderOptions, params, &usingCustomParseOptions);
  if (NS_FAILED(rv)) {
    return rv;
  }

  nsCOMPtr<nsIInputStream> stream;
  rv = ExtractData(type, params, getter_AddRefs(stream), fallbackToPNG);

  // If there are unrecognized custom parse options, we should fall back to
  // the default values for the encoder without any options at all.
  if (rv == NS_ERROR_INVALID_ARG && usingCustomParseOptions) {
    fallbackToPNG = false;
    rv = ExtractData(type, EmptyString(), getter_AddRefs(stream), fallbackToPNG);
  }

  NS_ENSURE_SUCCESS(rv, rv);

  // build data URL string
  if (fallbackToPNG)
    aDataURL = NS_LITERAL_STRING("data:image/png;base64,");
  else
    aDataURL = NS_LITERAL_STRING("data:") + type +
      NS_LITERAL_STRING(";base64,");

  uint64_t count;
  rv = stream->Available(&count);
  NS_ENSURE_SUCCESS(rv, rv);
  NS_ENSURE_TRUE(count <= UINT32_MAX, NS_ERROR_FILE_TOO_BIG);

  return Base64EncodeInputStream(stream, aDataURL, (uint32_t)count, aDataURL.Length());
}

// XXXkhuey the encoding should be off the main thread, but we're lazy.
void
HTMLCanvasElement::ToBlob(JSContext* aCx,
                          FileCallback& aCallback,
                          const nsAString& aType,
                          const Optional<JS::Handle<JS::Value> >& aParams,
                          ErrorResult& aRv)
{
  // do a trust check if this is a write-only canvas
  if (mWriteOnly && !nsContentUtils::IsCallerChrome()) {
    aRv.Throw(NS_ERROR_DOM_SECURITY_ERR);
    return;
  }

  nsAutoString type;
  aRv = nsContentUtils::ASCIIToLower(aType, type);
  if (aRv.Failed()) {
    return;
  }

  JS::Value encoderOptions = aParams.WasPassed()
                             ? aParams.Value()
                             : JS::UndefinedValue();

  nsAutoString params;
  bool usingCustomParseOptions;
  aRv = ParseParams(aCx, type, encoderOptions, params, &usingCustomParseOptions);
  if (aRv.Failed()) {
    return;
  }

#ifdef DEBUG
  if (mCurrentContext) {
    nsIntSize elementSize = GetWidthHeight();
    MOZ_ASSERT(elementSize.width == mCurrentContext->GetWidth());
    MOZ_ASSERT(elementSize.height == mCurrentContext->GetHeight());
  }
#endif

  bool fallbackToPNG = false;

  nsCOMPtr<nsIInputStream> stream;
  aRv = ExtractData(type, params, getter_AddRefs(stream), fallbackToPNG);
  // If there are unrecognized custom parse options, we should fall back to
  // the default values for the encoder without any options at all.
  if (aRv.ErrorCode() == NS_ERROR_INVALID_ARG && usingCustomParseOptions) {
    fallbackToPNG = false;
    aRv = ExtractData(type, EmptyString(), getter_AddRefs(stream), fallbackToPNG);
  }

  if (aRv.Failed()) {
    return;
  }

  if (fallbackToPNG) {
    type.AssignLiteral("image/png");
  }

  uint64_t imgSize;
  aRv = stream->Available(&imgSize);
  if (aRv.Failed()) {
    return;
  }
  if (imgSize > UINT32_MAX) {
    aRv.Throw(NS_ERROR_FILE_TOO_BIG);
    return;
  }

  void* imgData = nullptr;
  aRv = NS_ReadInputStreamToBuffer(stream, &imgData, imgSize);
  if (aRv.Failed()) {
    return;
  }

  // The DOMFile takes ownership of the buffer
  nsRefPtr<nsDOMMemoryFile> blob =
    new nsDOMMemoryFile(imgData, imgSize, type);

  JSContext* cx = nsContentUtils::GetCurrentJSContext();
  if (cx) {
    JS_updateMallocCounter(cx, imgSize);
  }

  nsRefPtr<ToBlobRunnable> runnable = new ToBlobRunnable(aCallback, blob);
  aRv = NS_DispatchToCurrentThread(runnable);
}

already_AddRefed<nsIDOMFile>
HTMLCanvasElement::MozGetAsFile(const nsAString& aName,
                                const nsAString& aType,
                                ErrorResult& aRv)
{
  nsCOMPtr<nsIDOMFile> file;
  aRv = MozGetAsFile(aName, aType, getter_AddRefs(file));
  return file.forget();
}

NS_IMETHODIMP
HTMLCanvasElement::MozGetAsFile(const nsAString& aName,
                                const nsAString& aType,
                                nsIDOMFile** aResult)
{
  OwnerDoc()->WarnOnceAbout(nsIDocument::eMozGetAsFile);

  // do a trust check if this is a write-only canvas
  if ((mWriteOnly) &&
      !nsContentUtils::IsCallerChrome()) {
    return NS_ERROR_DOM_SECURITY_ERR;
  }

  return MozGetAsFileImpl(aName, aType, aResult);
}

nsresult
HTMLCanvasElement::MozGetAsFileImpl(const nsAString& aName,
                                    const nsAString& aType,
                                    nsIDOMFile** aResult)
{
  bool fallbackToPNG = false;

  nsCOMPtr<nsIInputStream> stream;
  nsresult rv = ExtractData(aType, EmptyString(), getter_AddRefs(stream),
                            fallbackToPNG);
  NS_ENSURE_SUCCESS(rv, rv);

  nsAutoString type(aType);
  if (fallbackToPNG) {
    type.AssignLiteral("image/png");
  }

  uint64_t imgSize;
  rv = stream->Available(&imgSize);
  NS_ENSURE_SUCCESS(rv, rv);
  NS_ENSURE_TRUE(imgSize <= UINT32_MAX, NS_ERROR_FILE_TOO_BIG);

  void* imgData = nullptr;
  rv = NS_ReadInputStreamToBuffer(stream, &imgData, (uint32_t)imgSize);
  NS_ENSURE_SUCCESS(rv, rv);

  JSContext* cx = nsContentUtils::GetCurrentJSContext();
  if (cx) {
    JS_updateMallocCounter(cx, imgSize);
  }

  // The DOMFile takes ownership of the buffer
  nsRefPtr<nsDOMMemoryFile> file =
    new nsDOMMemoryFile(imgData, (uint32_t)imgSize, aName, type);

  file.forget(aResult);
  return NS_OK;
}

nsresult
HTMLCanvasElement::GetContextHelper(const nsAString& aContextId,
                                    nsICanvasRenderingContextInternal **aContext)
{
  NS_ENSURE_ARG(aContext);

  if (aContextId.EqualsLiteral("2d")) {
    Telemetry::Accumulate(Telemetry::CANVAS_2D_USED, 1);
    nsRefPtr<CanvasRenderingContext2D> ctx =
      new CanvasRenderingContext2D();

    ctx->SetCanvasElement(this);
    ctx.forget(aContext);
    return NS_OK;
  }
#ifdef MOZ_WEBGL
  if (WebGL2Context::IsSupported() &&
      aContextId.EqualsLiteral("experimental-webgl2"))
  {
    Telemetry::Accumulate(Telemetry::CANVAS_WEBGL_USED, 1);
    nsRefPtr<WebGL2Context> ctx = WebGL2Context::Create();

    if (ctx == nullptr) {
      return NS_ERROR_NOT_IMPLEMENTED;
    }

    ctx->SetCanvasElement(this);
    ctx.forget(aContext);
    return NS_OK;
  }
#endif

  NS_ConvertUTF16toUTF8 ctxId(aContextId);

  // check that ctxId is clamped to A-Za-z0-9_-
  for (uint32_t i = 0; i < ctxId.Length(); i++) {
    if ((ctxId[i] < 'A' || ctxId[i] > 'Z') &&
        (ctxId[i] < 'a' || ctxId[i] > 'z') &&
        (ctxId[i] < '0' || ctxId[i] > '9') &&
        (ctxId[i] != '-') &&
        (ctxId[i] != '_'))
    {
      // XXX ERRMSG we need to report an error to developers here! (bug 329026)
      return NS_OK;
    }
  }

  nsCString ctxString("@mozilla.org/content/canvas-rendering-context;1?id=");
  ctxString.Append(ctxId);

  nsresult rv;
  nsCOMPtr<nsICanvasRenderingContextInternal> ctx =
    do_CreateInstance(ctxString.get(), &rv);
  if (rv == NS_ERROR_OUT_OF_MEMORY) {
    *aContext = nullptr;
    return NS_ERROR_OUT_OF_MEMORY;
  }
  if (NS_FAILED(rv)) {
    *aContext = nullptr;
    // XXX ERRMSG we need to report an error to developers here! (bug 329026)
    return NS_OK;
  }

  ctx->SetCanvasElement(this);
  ctx.forget(aContext);
  return NS_OK;
}

nsresult
HTMLCanvasElement::GetContext(const nsAString& aContextId,
                              nsISupports** aContext)
{
  ErrorResult rv;
  *aContext = GetContext(nullptr, aContextId, JS::NullHandleValue, rv).get();
  return rv.ErrorCode();
}

static bool
IsContextIdWebGL(const nsAString& str)
{
  return str.EqualsLiteral("webgl") ||
         str.EqualsLiteral("experimental-webgl") ||
         str.EqualsLiteral("moz-webgl");
}

already_AddRefed<nsISupports>
HTMLCanvasElement::GetContext(JSContext* aCx,
                              const nsAString& aContextId,
                              JS::Handle<JS::Value> aContextOptions,
                              ErrorResult& rv)
{
  if (mCurrentContextId.IsEmpty()) {
    rv = GetContextHelper(aContextId, getter_AddRefs(mCurrentContext));
    if (rv.Failed() || !mCurrentContext) {
      return nullptr;
    }

    // Ensure that the context participates in CC.  Note that returning a
    // CC participant from QI doesn't addref.
    nsXPCOMCycleCollectionParticipant *cp = nullptr;
    CallQueryInterface(mCurrentContext, &cp);
    if (!cp) {
      mCurrentContext = nullptr;
      rv.Throw(NS_ERROR_FAILURE);
      return nullptr;
    }

    rv = UpdateContext(aCx, aContextOptions);
    if (rv.Failed()) {
      rv = NS_OK; // See bug 645792
      return nullptr;
    }
    mCurrentContextId.Assign(aContextId);
  }

  if (!mCurrentContextId.Equals(aContextId)) {
    if (IsContextIdWebGL(aContextId) &&
        IsContextIdWebGL(mCurrentContextId))
    {
      // Warn when we get a request for a webgl context with an id that differs
      // from the id it was created with.
      nsCString creationId = NS_LossyConvertUTF16toASCII(mCurrentContextId);
      nsCString requestId = NS_LossyConvertUTF16toASCII(aContextId);
      JS_ReportWarning(aCx, "WebGL: Retrieving a WebGL context from a canvas "
                            "via a request id ('%s') different from the id used "
                            "to create the context ('%s') is not allowed.",
                            requestId.get(),
                            creationId.get());
    }
    
    //XXX eventually allow for more than one active context on a given canvas
    return nullptr;
  }

  nsCOMPtr<nsICanvasRenderingContextInternal> context = mCurrentContext;
  return context.forget();
}

NS_IMETHODIMP
HTMLCanvasElement::MozGetIPCContext(const nsAString& aContextId,
                                    nsISupports **aContext)
{
  if(!nsContentUtils::IsCallerChrome()) {
    // XXX ERRMSG we need to report an error to developers here! (bug 329026)
    return NS_ERROR_DOM_SECURITY_ERR;
  }

  // We only support 2d shmem contexts for now.
  if (!aContextId.Equals(NS_LITERAL_STRING("2d")))
    return NS_ERROR_INVALID_ARG;

  if (mCurrentContextId.IsEmpty()) {
    nsresult rv = GetContextHelper(aContextId, getter_AddRefs(mCurrentContext));
    NS_ENSURE_SUCCESS(rv, rv);
    if (!mCurrentContext) {
      return NS_OK;
    }

    mCurrentContext->SetIsIPC(true);

    rv = UpdateContext(nullptr, JS::NullHandleValue);
    NS_ENSURE_SUCCESS(rv, rv);

    mCurrentContextId.Assign(aContextId);
  } else if (!mCurrentContextId.Equals(aContextId)) {
    //XXX eventually allow for more than one active context on a given canvas
    return NS_ERROR_INVALID_ARG;
  }

  NS_ADDREF (*aContext = mCurrentContext);
  return NS_OK;
}

nsresult
HTMLCanvasElement::UpdateContext(JSContext* aCx, JS::Handle<JS::Value> aNewContextOptions)
{
  if (!mCurrentContext)
    return NS_OK;

  nsIntSize sz = GetWidthHeight();

  nsresult rv = mCurrentContext->SetIsOpaque(GetIsOpaque());
  if (NS_FAILED(rv)) {
    mCurrentContext = nullptr;
    mCurrentContextId.Truncate();
    return rv;
  }

  rv = mCurrentContext->SetContextOptions(aCx, aNewContextOptions);
  if (NS_FAILED(rv)) {
    mCurrentContext = nullptr;
    mCurrentContextId.Truncate();
    return rv;
  }

  rv = mCurrentContext->SetDimensions(sz.width, sz.height);
  if (NS_FAILED(rv)) {
    mCurrentContext = nullptr;
    mCurrentContextId.Truncate();
    return rv;
  }

  return rv;
}

nsIntSize
HTMLCanvasElement::GetSize()
{
  return GetWidthHeight();
}

bool
HTMLCanvasElement::IsWriteOnly()
{
  return mWriteOnly;
}

void
HTMLCanvasElement::SetWriteOnly()
{
  mWriteOnly = true;
}

void
HTMLCanvasElement::InvalidateCanvasContent(const gfx::Rect* damageRect)
{
  // We don't need to flush anything here; if there's no frame or if
  // we plan to reframe we don't need to invalidate it anyway.
  nsIFrame *frame = GetPrimaryFrame();
  if (!frame)
    return;

  frame->MarkLayersActive(nsChangeHint(0));

  Layer* layer = nullptr;
  if (damageRect) {
    nsIntSize size = GetWidthHeight();
    if (size.width != 0 && size.height != 0) {

      gfx::Rect realRect(*damageRect);
      realRect.RoundOut();

      // then make it a nsIntRect
      nsIntRect invalRect(realRect.X(), realRect.Y(),
                          realRect.Width(), realRect.Height());

      layer = frame->InvalidateLayer(nsDisplayItem::TYPE_CANVAS, &invalRect);
    }
  } else {
    layer = frame->InvalidateLayer(nsDisplayItem::TYPE_CANVAS);
  }
  if (layer) {
    static_cast<CanvasLayer*>(layer)->Updated();
  }

  /*
   * Treat canvas invalidations as animation activity for JS. Frequently
   * invalidating a canvas will feed into heuristics and cause JIT code to be
   * kept around longer, for smoother animations.
   */
  nsCOMPtr<nsIGlobalObject> global =
    do_QueryInterface(OwnerDoc()->GetInnerWindow());

  if (global) {
    if (JSObject *obj = global->GetGlobalJSObject()) {
      js::NotifyAnimationActivity(obj);
    }
  }
}

void
HTMLCanvasElement::InvalidateCanvas()
{
  // We don't need to flush anything here; if there's no frame or if
  // we plan to reframe we don't need to invalidate it anyway.
  nsIFrame *frame = GetPrimaryFrame();
  if (!frame)
    return;

  frame->InvalidateFrame();
}

int32_t
HTMLCanvasElement::CountContexts()
{
  if (mCurrentContext)
    return 1;

  return 0;
}

nsICanvasRenderingContextInternal *
HTMLCanvasElement::GetContextAtIndex(int32_t index)
{
  if (mCurrentContext && index == 0)
    return mCurrentContext;

  return nullptr;
}

bool
HTMLCanvasElement::GetIsOpaque()
{
  return HasAttr(kNameSpaceID_None, nsGkAtoms::moz_opaque);
}

already_AddRefed<CanvasLayer>
HTMLCanvasElement::GetCanvasLayer(nsDisplayListBuilder* aBuilder,
                                  CanvasLayer *aOldLayer,
                                  LayerManager *aManager)
{
  if (!mCurrentContext)
    return nullptr;

  return mCurrentContext->GetCanvasLayer(aBuilder, aOldLayer, aManager);
}

bool
HTMLCanvasElement::ShouldForceInactiveLayer(LayerManager *aManager)
{
  return !mCurrentContext || mCurrentContext->ShouldForceInactiveLayer(aManager);
}

void
HTMLCanvasElement::MarkContextClean()
{
  if (!mCurrentContext)
    return;

  mCurrentContext->MarkContextClean();
}

NS_IMETHODIMP_(nsIntSize)
HTMLCanvasElement::GetSizeExternal()
{
  return GetWidthHeight();
}

NS_IMETHODIMP
HTMLCanvasElement::RenderContextsExternal(gfxContext *aContext, GraphicsFilter aFilter, uint32_t aFlags)
{
  if (!mCurrentContext)
    return NS_OK;

  return mCurrentContext->Render(aContext, aFilter, aFlags);
}

} // namespace dom
} // namespace mozilla
