(in-package :cl-user)

(defun foreign-symbols-to-c (output-pathname sorted-symbols)
  (with-open-file (output output-pathname
                          :direction :output
                          :if-exists :supersede)
    ;; Needed for uintptr_t. We use the raw uintptr_t as we don't want to have
    ;; to include any SBCL headers just to get at lispobj.
    (format output "#include <stdint.h>~%~%")

    ;; Write out the extern definitions. Everything is a void function (even
    ;; variables) because compilers don't like void variables. Remove
    ;; lisp_linkage_values as we need to write to it, so we should use the
    ;; actual type.
    (format output "extern void ~{~A()~^, ~};~%~%"
            (remove "lisp_linkage_values"
                    (mapcar #'first
                            (remove t sorted-symbols :key #'third))
                    :test #'equal))

    (format output "uintptr_t lisp_linkage_values[] = {~%")

    (format output "  ~D,~%" (length sorted-symbols))
    (dolist (symbol sorted-symbols)
      (destructuring-bind (name datap undefinedp) symbol
        (when datap
          ;; This is data, put -1 in to indicate that.
          (format output "  (uintptr_t)-1,~%"))
        (if undefinedp
            (format output "  (uintptr_t)0,~%")
            (format output "  (uintptr_t)&~A,~%" name))))
    (format output "};~%")))

(defun main (&optional (args (cdr sb-ext:*posix-argv*)))
  (foreign-symbols-to-c (second args)
                        (with-open-file (s (first args))
                          (read s))))

(eval-when (:execute)
  (let ((args (cdr sb-ext:*posix-argv*)))
    (when args
      (let ((*print-pretty* nil))
        (main args)))))
