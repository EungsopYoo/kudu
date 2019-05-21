// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
package org.apache.kudu.test;

import java.io.Closeable;
import java.io.IOException;

import com.google.common.base.Throwables;
import org.apache.logging.log4j.core.LogEvent;
import org.apache.logging.log4j.core.LoggerContext;
import org.apache.logging.log4j.core.appender.AbstractAppender;
import org.apache.logging.log4j.core.config.Property;
import org.apache.logging.log4j.core.layout.PatternLayout;
import org.apache.yetus.audience.InterfaceAudience;
import org.apache.yetus.audience.InterfaceStability;

/**
 * Test utility which wraps Log4j and captures all messages logged
 * while it is attached. This can be useful for asserting that a particular
 * message is (or is not) logged.
 */
@InterfaceAudience.Private
@InterfaceStability.Unstable
public class CapturingLogAppender extends AbstractAppender {
  // This is the standard layout used in Kudu tests.
  private static final PatternLayout LAYOUT = PatternLayout.newBuilder()
      .withPattern("%d{HH:mm:ss.SSS} [%p - %t] (%F:%L) %m%n")
      .build();

  private StringBuilder appended = new StringBuilder();

  public CapturingLogAppender() {
    super("CapturingLogAppender", /* filter */ null, LAYOUT,
        /* ignoreExceptions */ true, Property.EMPTY_ARRAY);
  }

  @Override
  public void append(LogEvent event) {
    appended.append(getLayout().toSerializable(event));
    if (event.getThrown() != null) {
      appended.append(Throwables.getStackTraceAsString(event.getThrown()));
      appended.append("\n");
    }
  }

  /**
   * @return all of the appended messages captured thus far, joined together.
   */
  public String getAppendedText() {
    return appended.toString();
  }

  /**
   * Temporarily attach the capturing appender to the Log4j root logger.
   * This can be used in a 'try-with-resources' block:
   * <code>
   *   try (Closeable c = capturer.attach()) {
   *     ...
   *   }
   * </code>
   */
  public Closeable attach() {
    LoggerContext.getContext(false).getRootLogger().addAppender(this);
    return new Closeable() {
      @Override
      public void close() throws IOException {
        LoggerContext.getContext(false).getRootLogger().removeAppender(CapturingLogAppender.this);
      }
    };
  }
}
