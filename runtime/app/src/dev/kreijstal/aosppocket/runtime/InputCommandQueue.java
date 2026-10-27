// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import java.util.concurrent.ThreadPoolExecutor;

/** Keep the newest unsent motion sample without crossing an input boundary. */
final class InputCommandQueue {
    static void execute(ThreadPoolExecutor worker, Runnable command, boolean motion) {
        synchronized (worker) {
            if (motion && !worker.getQueue().isEmpty()) {
                Object[] waiting = worker.getQueue().toArray();
                if (waiting.length > 0 && waiting[waiting.length - 1] instanceof Pending) {
                    Pending last = (Pending) waiting[waiting.length - 1];
                    if (last.motion && !last.started) {
                        last.command = command;
                        return;
                    }
                }
            }
            worker.execute(new Pending(worker, command, motion));
        }
    }

    private static final class Pending implements Runnable {
        final ThreadPoolExecutor worker;
        final boolean motion;
        Runnable command;
        boolean started;

        Pending(ThreadPoolExecutor worker, Runnable command, boolean motion) {
            this.worker = worker;
            this.command = command;
            this.motion = motion;
        }

        @Override public void run() {
            Runnable next;
            synchronized (worker) {
                started = true;
                next = command;
                command = null;
            }
            next.run();
        }
    }
}
