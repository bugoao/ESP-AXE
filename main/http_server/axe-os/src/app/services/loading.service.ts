import { Injectable } from '@angular/core';
import { BehaviorSubject, defer, finalize, Observable } from 'rxjs';

@Injectable({
  providedIn: 'root'
})
export class LoadingService {

  public loading$: BehaviorSubject<boolean> = new BehaviorSubject(false);
  private pendingRequests = 0;

  constructor() { }

  public lockUIUntilComplete() {
    return <T>(source: Observable<T>): Observable<T> => {
      return defer(() => {
        this.pendingRequests++;
        this.loading$.next(true);
        return source.pipe(finalize(() => {
          this.pendingRequests--;
          this.loading$.next(this.pendingRequests > 0);
        }));
      });
    }
  }
}
