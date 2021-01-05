(defpackage #:sb-dump-linkage-info
  (:use #:cl)
  (:export #:dump-to-file))

(in-package #:sb-dump-linkage-info)

(defparameter *libdl-symbols* '("dladdr" "dlclose" "dlerror" "dlopen" "dlsym"))

(defun dump-to-file (pn &key (remove-symbols *libdl-symbols*))
  (let ((ht (car sb-sys:*linkage-info*))
        (undefined (cdr sb-sys:*linkage-info*))
        out)
    (loop
      :for key :being :the :hash-keys :in ht
      :for datap := (listp key)
      :for name := (if datap (first key) key)
      :unless (or (member key undefined :test #'equal)
                  (member name remove-symbols
                          :test #'equal))
        :do (push (list name datap) out))
    (ensure-directories-exist pn)
    (with-open-file (s pn :direction :output :if-exists :supersede)
      (let ((*print-pretty* nil))
        (prin1 (sort out #'string< :key #'first) s))
      (terpri s)))
  pn)
