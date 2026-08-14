/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @generated SignedSource<<df482e604eeb99fc55b3080f4a18197c>>
 *
 * This file was translated from Flow by scripts/js-api/build-types/index.js.
 * Original file: packages/react-native/src/private/webapis/dom/events/EventTarget.js
 */

import Event from "./Event";
export type EventCallback = (event: Event) => void;
export type EventHandler = {
  handleEvent(event: Event): void;
};
export type EventListener = EventCallback | EventHandler;
export type EventListenerOptions = Readonly<{
  capture?: boolean;
}>;
export type AddEventListenerOptions = Readonly<Omit<EventListenerOptions, keyof {
  passive?: boolean;
  once?: boolean;
  signal?: AbortSignal;
}> & {
  passive?: boolean;
  once?: boolean;
  signal?: AbortSignal;
}>;
declare class EventTarget {
  addEventListener(type: string, callback: EventListener | null, optionsOrUseCapture?: AddEventListenerOptions | boolean): void;
  removeEventListener(type: string, callback: EventListener, optionsOrUseCapture?: EventListenerOptions | boolean): void;
  dispatchEvent(event: Event): boolean;
}
export default EventTarget;
