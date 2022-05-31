/*
 * Copyright (C) 2016 Yusuke Suzuki <utatane.tea@gmail.com>.
 * Copyright (C) 2017 Caio Lima <ticaiolima@gmail.com>.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// @internal

@globalPrivate
function toIntegerOrInfinity(target)
{
    "use strict";

    var numberValue = +target;

    // isNaN(numberValue) or 0
    if (numberValue !== numberValue || !numberValue)
        return 0;
    return @trunc(numberValue);
}

@globalPrivate
function toLength(target)
{
    "use strict";

    var length = @toIntegerOrInfinity(target);
    // originally Math.min(Math.max(length, 0), maxSafeInteger));
    return +(length > 0 ? (length < @MAX_SAFE_INTEGER ? length : @MAX_SAFE_INTEGER) : 0);
}

@globalPrivate
@getter
@overriddenName="get [Symbol.species]"
function speciesGetter()
{
    "use strict";
    return this;
}

@globalPrivate
function speciesConstructor(obj, defaultConstructor)
{
    "use strict";

    var constructor = obj.constructor;
    if (constructor === @undefined)
        return defaultConstructor;
    if (!@isObject(constructor))
        @throwTypeError("|this|.constructor is not an Object or undefined");
    constructor = constructor.@@species;
    if (@isUndefinedOrNull(constructor))
        return defaultConstructor;
    if (@isConstructor(constructor))
        return constructor;
    @throwTypeError("|this|.constructor[Symbol.species] is not a constructor");
}

@globalPrivate
function enqueueJob(job) {
    "use strict";

    
    switch (@argumentCount()) {
      case 1: {
        @microtaskQueue.push([job]);
        break;
      }
      case 2: {
        @microtaskQueue.push([job, @argument(1)]);
        break;
      }
      case 3: {
        @microtaskQueue.push([job, @argument(1), @argument(2)]);
        break;
      }
      case 4: {
        @microtaskQueue.push([job, @argument(1), @argument(2), @argument(3)]);
        break;
      }
      case 5: {
        @microtaskQueue.push([job, @argument(1), @argument(2), @argument(3), @argument(4)]);
        break;
      }
      case 0: {
        break;
      }
      default: {
        @microtaskQueue.push(@Array.prototype.slice.@call(arguments));
        break;
      }
    }
}

function drainMicrotaskQueue() {
    "use strict";

    var any = false;
    while (true) {
        var item = @microtaskQueue.shift();
        if (item === @undefined) return any;
        
        var job = item[0];

        switch (job) {
          case @promiseResolveThenableJobWithoutPromiseFast: {
            @promiseResolveThenableJobWithoutPromiseFast(item[1], item[2], item[3]);
            break;
          }
          case @promiseResolveThenableJobFast: {
            @promiseResolveThenableJobFast(item[1], item[2]);
            break;
          }
          case @promiseReactionJob: {
            @promiseReactionJob(item[1], item[2], item[3], item[4]);
            break;
          }
          case @promiseReactionJobWithoutPromise: {
            @promiseReactionJobWithoutPromise(item[1], item[2]);
            break;
          }
          case @promiseResolveThenableJob: {
            @promiseResolveThenableJob(item[1], item[2], item[3]);
            break;
          }
          default: {
            // using .@apply was 20% slower
            // I saw calls to variadic arguments in Instruments
            switch (@toLength(item.length)) {
              case 1: {
                job();
                break;
              }
              case 2: {
                job(item[1]);
                break;
              }
              case 3: {
                job(item[1], item[2]);
                break;
              }
              case 4: {
                job(item[1], item[2], item[3]);
                break;
              }
              case 5: {
                job(item[1], item[2], item[3], item[4]);
                break;
              }
              default: {
                job.@apply(null, item.slice(1));
                break;
              }
            }
            break;
          }
        }
        
        any = true;
    }

    return any;
}


@globalPrivate
function initializeMicrotaskQueue() {
    'use strict';

    class Denqueue {
      constructor() {
        this._head = 0;
        this._tail = 0;
        // this._capacity = 0;
        this._capacityMask = 0x3;
        this._list = @newArrayWithSize(4);
      }

      size() {
        if (this._head === this._tail) return 0;
        if (this._head < this._tail) return this._tail - this._head;
        else return this._capacityMask + 1 - (this._head - this._tail);
      }

      shift() {
        var head = this._head;
        if (head === this._tail) return @undefined;
        var item = this._list[head];
        @putByValDirect(this._list, head, @undefined);
        this._head = (head + 1) & this._capacityMask;
        if (head < 2 && this._tail > 10000 && this._tail <= this._list.length >>> 2) this._shrinkArray();
        return item;
      }

      push(item) {
        var tail = this._tail;
        @putByValDirect(this._list, tail, item);
        this._tail = (tail + 1) & this._capacityMask;
        if (this._tail === this._head) {
          this._growArray();
        }
        // if (this._capacity && this.size() > this._capacity) {
          // this.shift();
        // }
      }

      _copyArray(fullCopy) {
        var list = this._list;
        var len = @toLength(list.length);
        
        if (fullCopy || this._head > this._tail) {
          var _head = @toLength(this._head);
          var _tail = @toLength(this._tail);
          var total = @toLength((len - _head) + _tail);
          var array = @newArrayWithSize(total);
          var j = 0;
          for (var i = _head; i < len; i++) @putByValDirect(array, j++, list[i]);
          for (var i = 0; i < _tail; i++) @putByValDirect(array, j++, list[i]);
          return array;
        } else {
          return @Array.prototype.slice.@call(list, this._head, this._tail);
        }
      }

      _growArray() {
        if (this._head) {
          // copy existing data, head to end, then beginning to tail.
          this._list = this._copyArray(true);
          this._head = 0;
        }
      
        // head is at 0 and array is now full, safe to extend
        this._tail = @toLength(this._list.length);
      
        this._list.length <<= 1;
        this._capacityMask = (this._capacityMask << 1) | 1;
      }

      shrinkArray() {
        this._list.length >>>= 1;
        this._capacityMask >>>= 1;
      }
    }
    
    return new Denqueue();
}