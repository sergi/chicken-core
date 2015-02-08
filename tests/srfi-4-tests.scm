;;;; srfi-4-tests.scm


(use srfi-1 srfi-4 ports)
(import-for-syntax chicken)

(define-syntax test1
  (er-macro-transformer
   (lambda (x r c)
     (let* ((t (strip-syntax (cadr x)))
	    (name (symbol->string (strip-syntax t)))
	    (min (caddr x))
	    (max (cadddr x)))
       (define (conc op)
	 (string->symbol (string-append name op)))
       `(let ((x (,(conc "vector") 100 101)))
	  (assert (eqv? 100 (,(conc "vector-ref") x 0)))
	  (assert (,(conc "vector?") x))
	  (assert (number-vector? x))
	  (,(conc "vector-set!") x 1 99)
	  (assert (eqv? 99 (,(conc "vector-ref") x 1)))
	  (assert (= 2 (,(conc "vector-length") x)))
	  (assert
	   (every eqv?
		  '(100 99)
		  (,(conc "vector->list") x))))))))

(test1 u8 0 255)
(test1 u16 0 65535)
(test1 u32 0 4294967295)
(test1 s8 -128 127)
(test1 s16 -32768 32767)
(test1 s32 -2147483648 2147483647)

(define-syntax test2
  (er-macro-transformer
   (lambda (x r c)
     (let* ((t (strip-syntax (cadr x)))
	    (name (symbol->string (strip-syntax t))))
       (define (conc op)
	 (string->symbol (string-append name op)))
       `(let ((x (,(conc "vector") 100 101.0)))
	  (assert (eqv? 100.0 (,(conc "vector-ref") x 0)))
	  (assert (eqv? 101.0 (,(conc "vector-ref") x 1)))
	  (assert (,(conc "vector?") x))
	  (assert (number-vector? x))
	  (,(conc "vector-set!") x 1 99)
	  (assert (eqv? 99.0 (,(conc "vector-ref") x 1)))
	  (assert (= 2 (,(conc "vector-length") x)))
	  (assert
	   (every eqv?
		  '(100.0 99.0)
		  (,(conc "vector->list") x))))))))

(test2 f32)
(test2 f64)

;; Test implicit quoting/self evaluation
(assert (equal? #u8(1 2 3) '#u8(1 2 3)))
(assert (equal? #s8(-1 2 3) '#s8(-1 2 3)))
(assert (equal? #u16(1 2 3) '#u16(1 2 3)))
(assert (equal? #s16(-1 2 3) '#s16(-1 2 3)))
(assert (equal? #u32(1 2 3) '#u32(1 2 3)))
(assert (equal? #s32(-1 2 3) '#s32(-1 2 3)))
(assert (equal? #f32(1 2 3) '#f32(1 2 3)))
(assert (equal? #f64(-1 2 3) '#f64(-1 2 3)))

;; Ticket #1124: read-u8vector! w/o length, dest smaller than source.
(let ((input (open-input-string "abcdefghijklmnopqrstuvwxyz"))
      (u8vec (make-u8vector 10)))
  (assert (= 10 (read-u8vector! #f u8vec input)))
  (assert (equal? u8vec #u8(97 98 99 100 101 102 103 104 105 106)))
  (assert (= 5  (read-u8vector! #f u8vec input 5)))
  (assert (equal? u8vec #u8(97 98 99 100 101 107 108 109 110 111)))
  (assert (= 5  (read-u8vector! 5  u8vec input)))
  (assert (equal? u8vec #u8(112 113 114 115 116 107 108 109 110 111)))
  (assert (= 6  (read-u8vector! 10 u8vec input)))
  (assert (equal? u8vec #u8(117 118 119 120 121 122 108 109 110 111))))

(assert (string=?
	 "abc"
	 (with-output-to-string
	   (lambda ()
	     (write-u8vector #u8(97 98 99))))))

(assert (string=?
	 "bc"
	 (with-output-to-string
	   (lambda ()
	     (write-u8vector #u8(97 98 99) (current-output-port) 1)))))

(assert (string=?
	 "a"
	 (with-output-to-string
	   (lambda ()
	     (write-u8vector #u8(97 98 99) (current-output-port) 0 1)))))

(assert (string=?
	 "b"
	 (with-output-to-string
	   (lambda ()
	     (write-u8vector #u8(97 98 99) (current-output-port) 1 2)))))

(assert (string=?
	 ""
	 (with-output-to-string
	   (lambda ()
	     (write-u8vector #u8())))))